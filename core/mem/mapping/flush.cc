/*
 * Invalidation, and the epoch counter that lets a client detach without one.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <atomic>

#include <osv/align.hh>
#include <osv/mem/mapping.hh>

namespace mem {
namespace mapping {

static std::atomic<uint64_t> epoch;

uint64_t flush_epoch()
{
    return epoch.load(std::memory_order_acquire);
}

void flush_local(range r)
{
    if (r.size() > flush_batch * page_size) {
        tlb_flush_local();
        return;
    }
    for (uintptr_t va = r.start; va < r.end; va += page_size) {
        tlb_flush_page(va);
    }
}

// Naming each address in turn, on every cpu.
// Does not move the epoch.
void flush_range(range r)
{
    size_t pages = align_up(r.size(), page_size) / page_size;
    if (pages > flush_batch) {
        flush_all();
        return;
    }
    uintptr_t va[flush_batch];
    uintptr_t a = align_down(r.start, page_size);
    for (size_t i = 0; i < pages; i++, a += page_size) {
        va[i] = a;
    }
    tlb_flush_pages_all(va, pages);
}

void flush_all()
{
    tlb_flush_all();
    epoch.fetch_add(1, std::memory_order_acq_rel);
}

void pending_invalidation::add(uintptr_t addr)
{
    if (count == flush_batch) {
        all = true;
        return;
    }
    va[count++] = addr;
}

void pending_invalidation::invalidate()
{
    // Need to be +2 in case a flush happened while accumulating addresses.
    if ((count || all) && flush_epoch() < epoch + 2) {
        if (all) {
            flush_all();
        } else {
            tlb_flush_pages_all(va, count);
        }
    }
    count = 0;
    all = false;
    epoch = 0;
}

}
}
