/*
 * miniext block I/O.
 *
 * This is the layer that would otherwise be a separate block-device
 * abstraction; folding it into the filesystem is deliberate. There is no
 * buffer cache: reads go to the device and writes land on it immediately, so
 * there is no dirty-writeback ordering to get wrong.
 *
 * Callers hand us DMA-capable memory (see scratch in internal.hh), which is why
 * the bounce-buffer path OSv's libext needed never arises here.
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

    // create_io_queue asserts on a null cpu despite its doc comment saying
    // nullptr means "current"; pass one explicitly.
    _q = static_cast<nvme::io_queue_pair *>(
        drv->create_io_queue(16, sched::cpus[0]));
    if (!_q) {
        printf("miniext: could not create an NVMe I/O queue\n");
        return -EIO;
    }
    return 0;
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
    // The driver owns the queue; there is no teardown path we need here.
    _q = nullptr;
}

int device::submit(void *buf, uint64_t block, uint32_t count, bool write)
{
    if (!_q) {
        return -ENODEV;
    }

    const uint64_t byte_off = block * static_cast<uint64_t>(_block_size);
    const uint32_t byte_len = count * _block_size;

    if ((block + count) * static_cast<uint64_t>(_lbas_per_block) > _lba_count) {
        printf("miniext: I/O past end of namespace (block %lu count %u)\n",
               (unsigned long)block, count);
        return -EIO;
    }

    SCOPE_LOCK(_lock);

    io_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request returns 1 when queued and 0 when the submission queue was
    // full -- 0 means nothing was submitted, so retry rather than wait forever.
    int rc;
    while ((rc = _q->submit_request(1, buf, byte_off, byte_len, io_complete, &req,
                                    0, write ? nvme::WRITE : nvme::READ)) == 0) {
        sched::thread::yield();
    }
    if (rc != 1) {
        req.waiter.clear();
        printf("miniext: submit_request failed (%d)\n", rc);
        return -EIO;
    }

    // The MSI-X handler drains completions and wakes us. A completion error is
    // fatal inside the driver (it asserts), so reaching here means success.
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
    if (!_q) {
        return -ENODEV;
    }

    SCOPE_LOCK(_lock);

    io_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request asserts nlb >= 1 before it looks at the opcode, so a FLUSH
    // still has to present a non-zero length even though submit_flush_cmd()
    // ignores the address and length entirely. Hand it one block.
    int rc;
    while ((rc = _q->submit_request(1, nullptr, 0, _block_size, io_complete, &req,
                                    0, nvme::FLUSH)) == 0) {
        sched::thread::yield();
    }
    if (rc != 1) {
        req.waiter.clear();
        return -EIO;
    }

    sched::thread::wait_until([&req] { return req.done; });
    req.waiter.clear();
    return 0;
}

} // namespace miniext
