#include <osv/bootargs.hh>
#include <osv/drivers_config.h>

#include <cstring>

#if CONF_drivers_nvme
#include <osv/contiguous_alloc.hh>
#include <osv/sched.hh>

#include "drivers/nvme.hh"
#include "drivers/nvme-queue.hh"
#endif

namespace osv {

namespace {

#if CONF_drivers_nvme

struct read_request {
    sched::thread_handle waiter;
    volatile bool done = false;
};

void read_complete(void *ctx, const nvme_sq_entry_t *)
{
    auto *req = static_cast<read_request *>(ctx);
    req->done = true;
    req->waiter.wake_from_kernel_or_with_irq_disabled();
}

// Read one raw sector off the boot disk (NVMe controller 0). 
// Returns false when there is no such controller (absence of arguments).
bool read_boot_sector(unsigned lba, void *buf, unsigned bytes)
{
    auto *drv = nvme::nvme_driver::get_nvme_device(0);
    if (!drv) {
        return false;
    }
    auto ns = drv->_ns_data.find(1);
    if (ns == drv->_ns_data.end() || ns->second->blocksize == 0) {
        return false;
    }
    const unsigned lba_size = ns->second->blocksize;
    if (bytes % lba_size != 0) {
        return false;
    }

    // create_io_queue asserts on a null cpu despite its doc comment.
    auto *q = static_cast<nvme::io_queue_pair *>(
        drv->create_io_queue(4, sched::cpus[0]));
    if (!q) {
        return false;
    }

    read_request req;
    req.waiter.reset(*sched::thread::current());

    // submit_request's 3rd and 4th arguments are a byte offset and a byte
    // length despite being named lba/lba_count, and it returns 1 on success,
    // 0 when the queue was full and nothing was submitted.
    int rc;
    while ((rc = q->submit_request(1, buf, static_cast<uint64_t>(lba) * lba_size,
                                   bytes, read_complete, &req, 0,
                                   nvme::READ)) == 0) {
        sched::thread::yield();
    }
    if (rc != 1) {
        req.waiter.clear();
        return false;
    }
    sched::thread::wait_until([&req] { return req.done; });
    req.waiter.clear();
    return true;
}

#endif /* CONF_drivers_nvme */

std::string read_bootargs()
{
#if CONF_drivers_nvme
    // The read has to land in DMA-capable memory, so it cannot go straight
    // into a stack buffer.
    auto *buf = static_cast<uint8_t *>(
        memory::alloc_phys_contiguous_aligned(512, 512));
    if (!buf) {
        return {};
    }
    memset(buf, 0, 512);

    std::string out;
    if (read_boot_sector(bootargs_lba, buf, 512)) {
        auto *blk = reinterpret_cast<bootargs_block *>(buf);
        if (memcmp(blk->magic, bootargs_magic, sizeof(bootargs_magic)) == 0) {
            uint32_t len = blk->length;
            if (len > bootargs_max - 1) {
                len = bootargs_max - 1;
            }
            blk->args[len] = '\0';
            out.assign(blk->args, len);
        }
    }
    memory::free_phys_contiguous_aligned(buf);
    return out;
#else
    return {};
#endif
}

} // namespace

std::string bootargs()
{
    // Read once: the block cannot change while we are running, and this is on
    // the path to the application's entry point.
    static bool loaded = false;
    static std::string cached;
    if (!loaded) {
        cached = read_bootargs();
        loaded = true;
    }
    return cached;
}

std::vector<std::string> bootargs_split(const std::string &line)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_word = false;
    char quote = '\0';

    for (char c : line) {
        if (quote) {
            if (c == quote) {
                quote = '\0';
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;         // "" is a real, empty argument
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_word) {
                out.push_back(cur);
                cur.clear();
                in_word = false;
            }
            continue;
        }
        cur += c;
        in_word = true;
    }
    if (in_word) {
        out.push_back(cur);
    }
    return out;
}

} // namespace osv
