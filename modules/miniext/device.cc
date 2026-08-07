/*
 * miniext block I/O.
 *
 * This is the layer that would otherwise be a separate block-device
 * abstraction; folding it into the filesystem is deliberate. There is no
 * buffer cache: reads go to the device and writes land on it immediately, so
 * there is no dirty-writeback ordering to get wrong.
 *
 * One NVMe I/O queue per vCPU, so concurrent readers and writers do not
 * serialise behind each other. The per-queue lock covers only the submission
 * itself; the wait for completion happens with no lock held, which is what lets
 * requests overlap.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <osv/contiguous_alloc.hh>
#include <osv/sched.hh>

#include "drivers/nvme.hh"
#include "drivers/nvme-queue.hh"
#include "internal.hh"

namespace miniext {

// Queue depth per vCPU queue. 16 matches what the driver's own callers use;
// deeper would allow more in-flight requests per CPU at the cost of more
// pinned command slots.
static const int NVME_QUEUE_DEPTH = 16;

// One outstanding request; the completion callback runs in the MSI-X handler,
// so it does the minimum: flag and wake. This mirrors what the driver's own
// admin queue does (drivers/nvme-queue.cc:563-565, 591-593).
namespace {
struct io_request {
    sched::thread_handle waiter;
    volatile bool done = false;
};

void io_complete(void *ctx, const nvme_sq_entry_t *)
{
    auto *req = static_cast<io_request *>(ctx);
    req->done = true;
    req->waiter.wake_from_kernel_or_with_irq_disabled();
}
} // namespace

int device::open(int nvme_id)
{
    auto *drv = nvme::nvme_driver::get_nvme_device(nvme_id);
    if (!drv) {
        printf("miniext: no NVMe controller with id %d\n", nvme_id);
        return -ENODEV;
    }

    auto ns = drv->_ns_data.find(1);
    if (ns == drv->_ns_data.end()) {
        printf("miniext: NVMe controller %d has no namespace 1\n", nvme_id);
        return -ENODEV;
    }

    _lba_size = ns->second->blocksize;
    _lba_count = ns->second->blockcount;
    if (_lba_size == 0) {
        printf("miniext: NVMe namespace reports a zero LBA size\n");
        return -EINVAL;
    }
    // Address raw LBAs until the superblock tells us the real block size.
    _block_size = _lba_size;
    _lbas_per_block = 1;

    // One queue per vCPU, each with its completion interrupt pinned to that
    // CPU. create_io_queue asserts on a null cpu despite its doc comment saying
    // nullptr means "current", so the CPU is always passed explicitly.
    //
    // The controller may hand out fewer queues than we ask for (the driver caps
    // MSI-X vectors at min(msix_entries, ncpus + 1), one of which is the admin
    // queue). Whatever we get, pick() maps CPUs onto it, so fewer queues costs
    // throughput but never correctness.
    for (size_t i = 0; i < sched::cpus.size(); i++) {
        auto *qp = static_cast<nvme::io_queue_pair *>(
            drv->create_io_queue(NVME_QUEUE_DEPTH, sched::cpus[i]));
        if (!qp) {
            break;
        }
        auto slot = std::unique_ptr<queue>(new queue());
        slot->q = qp;
        _queues.push_back(std::move(slot));
    }

    if (_queues.empty()) {
        printf("miniext: could not create any NVMe I/O queue\n");
        return -EIO;
    }
    if (_queues.size() < sched::cpus.size()) {
        printf("miniext: %zu I/O queues for %zu vCPUs; some will share\n",
               _queues.size(), sched::cpus.size());
    }
    return 0;
}

// The queue for the CPU we are running on. A thread can migrate between
// picking a queue and submitting on it -- harmless, since the per-queue lock
// makes any queue safe for any submitter; the pinning is a locality hint.
device::queue &device::pick()
{
    unsigned id = sched::cpu::current()->id;
    return *_queues[id % _queues.size()];
}

int device::set_block_size(uint32_t block_size)
{
    if (_lba_size == 0 || block_size % _lba_size != 0) {
        printf("miniext: fs block size %u is not a multiple of the %u-byte LBA\n",
               block_size, _lba_size);
        return -EINVAL;
    }
    _block_size = block_size;
    _lbas_per_block = block_size / _lba_size;
    return 0;
}

void device::close()
{
    // The driver owns the queues; there is no teardown path we need here.
    _queues.clear();
}

int device::submit(void *buf, uint64_t block, uint32_t count, bool write)
{
    if (_queues.empty()) {
        return -ENODEV;
    }

    const uint64_t byte_off = block * static_cast<uint64_t>(_block_size);
    const uint32_t byte_len = count * _block_size;

    if ((block + count) * static_cast<uint64_t>(_lbas_per_block) > _lba_count) {
        printf("miniext: I/O past end of namespace (block %lu count %u)\n",
               (unsigned long)block, count);
        return -EIO;
    }

    queue &qu = pick();

    io_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request returns 1 when queued and 0 when the submission queue was
    // full -- 0 means nothing was submitted. Retry, but drop the lock first so
    // the in-flight requests ahead of us can complete and free a slot.
    for (;;) {
        int rc;
        {
            SCOPE_LOCK(qu.lock);
            rc = qu.q->submit_request(1, buf, byte_off, byte_len, io_complete,
                                      &req, 0, write ? nvme::WRITE : nvme::READ);
        }
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            req.waiter.clear();
            printf("miniext: submit_request failed (%d)\n", rc);
            return -EIO;
        }
        sched::thread::yield();
    }

    // Waiting with no lock held is the point: other threads keep submitting on
    // this queue while we are parked here. The MSI-X handler drains completions
    // and wakes us. A completion error is fatal inside the driver (it asserts),
    // so reaching here means success.
    sched::thread::wait_until([&req] { return req.done; });
    req.waiter.clear();
    return 0;
}

int device::read(void *buf, uint64_t block, uint32_t count)
{
    return submit(buf, block, count, false);
}

int device::write(const void *buf, uint64_t block, uint32_t count)
{
    return submit(const_cast<void *>(buf), block, count, true);
}

int device::flush()
{
    if (_queues.empty()) {
        return -ENODEV;
    }

    // A flush commits the controller's volatile write cache for the whole
    // namespace, so issuing it on one queue covers writes submitted on all of
    // them -- but only those that have already completed, which is why callers
    // must have collected their writes before calling this.
    queue &qu = pick();

    io_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request asserts nlb >= 1 before it looks at the opcode, so a FLUSH
    // still has to present a non-zero length even though submit_flush_cmd()
    // ignores the address and length entirely. Hand it one block.
    for (;;) {
        int rc;
        {
            SCOPE_LOCK(qu.lock);
            rc = qu.q->submit_request(1, nullptr, 0, _block_size, io_complete,
                                      &req, 0, nvme::FLUSH);
        }
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            req.waiter.clear();
            return -EIO;
        }
        sched::thread::yield();
    }

    sched::thread::wait_until([&req] { return req.done; });
    req.waiter.clear();
    return 0;
}

} // namespace miniext
