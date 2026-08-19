/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * Whole-range page table mapping operations.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>
#include <string.h>

#include "internal.hh"

namespace mem {
namespace mapping {

namespace {

range page_align(range r)
{
    return {align_down(r.start, page_size), align_up(r.end, page_size)};
}

// Clear every leaf in the range, handing what was there to fn.
template <typename Fn>
bool clear_range(range r, walk_result &res, bool reclaim, Fn fn)
{
    bool changed = false;
    walk_opts o{false, true, reclaim, max_leaf_level};
    res = walk_range(r, o, [&](pte_ref e, uintptr_t va) {
        pte old = e.exchange(0);
        if (!pte_empty(old)) {
            changed = true;
            fn(va, pte_addr(old, e.level()), e.size());
        }
        return true;
    });
    return changed || res.split || res.count;
}

// Check if the range contains any mapped addresses.
bool anything_mapped(range r)
{
    bool taken = false;
    walk_opts o{false, false, false, max_leaf_level};
    walk_range(r, o, [&](pte_ref e, uintptr_t) {
        taken = !e.empty();
        return !taken;
    });
    return taken;
}

// Check if the range contains any other addresses than 
// the ones that would be mapped by the given physical address.
bool conflicts_with_mapping(range r, phys_addr phys)
{
    bool clash = false;
    uintptr_t start = r.start;
    walk_opts o{false, false, false, max_leaf_level};
    walk_range(r, o, [&](pte_ref e, uintptr_t va) {
        pte old = e.read();
        if (pte_empty(old)) {
            return true;
        }
        clash = pte_addr(old, e.level()) != phys + (va - start);
        return !clash;
    });
    return clash;
}

} // namespace

// Write a leaf for every address in "write", using the "phys" frames.
static bool write_leaves(range write, uintptr_t origin, phys_addr phys,
                         unsigned perm, mattr ma, bool keep)
{
    unsigned level = max_leaf_level;
    while (level > 0 && ((origin ^ phys) & (level_size(level) - 1))) {
        --level;
    }

    walk_opts o{true, true, false, level};
    auto res = walk_range(write, o, [=](pte_ref e, uintptr_t va) {
        if (!keep || e.empty()) {
            e.write(e.leaf_for(phys + (va - origin), perm, ma));
        }
        return true;
    });
    pte_barrier();
    return res.complete;
}

bool attach(range r, phys_addr phys, unsigned perm, mattr ma)
{
    r = page_align(r);
    if (r.empty()) {
        return true;
    }
    if (anything_mapped(r)) {
        return false;
    }
    return write_leaves(r, r.start, phys, perm, ma, false);
}

bool attach_missing(range r, phys_addr phys, unsigned perm, size_t slop, mattr ma)
{
    r = page_align(r);
    if (r.empty()) {
        return true;
    }
    if (conflicts_with_mapping(r, phys)) {
        return false;
    }
    size_t grain = std::min(std::max(slop, page_size), level_size(max_leaf_level));
    range m = {align_down(r.start, grain), align_up(r.end, grain)};
    return write_leaves(m, r.start, phys, perm, ma, true);
}

// Unmap the range, and invalidate the TLB for it.
void detach(range r)
{
    pending_invalidation stale;
    walk_result res;
    clear_range(page_align(r), res, true, [&](uintptr_t va, phys_addr, size_t) {
        stale.add(va);
    });
    if (res.count) {
        stale.all = true;
    }
    stale.epoch = flush_epoch();
    stale.invalidate();
    res.settle();
}

// Unmap the range, but do not invalidate the TLB.
void detach_deferred(range r, pending_invalidation &stale)
{
    walk_result res;
    clear_range(page_align(r), res, false, [&](uintptr_t a, phys_addr, size_t) {
        stale.add(a);
    });
    stale.epoch = flush_epoch();
}

void protect(range r, unsigned perm)
{
    pending_invalidation stale;
    walk_opts o{false, true, false, max_leaf_level};
    auto res = walk_range(page_align(r), o, [&](pte_ref e, uintptr_t va) {
        pte old = e.read();
        if (pte_empty(old)) {
            return true;
        }
        pte now = pte_with_perm(old, perm);
        if (now != old) {
            e.write(now);
            if (pte_perm_change_needs_flush(pte_perm(old), perm)) {
                // Flush only if needed (arch specific).
                stale.add(va);
            }
        }
        return true;
    });
    pte_barrier();
    if (res.split) {
        stale.all = true;
    }
    stale.epoch = flush_epoch();
    stale.invalidate();
}

void split(range r)
{
    walk_opts o{false, true, false, 0};
    auto res = walk_range(page_align(r), o, [](pte_ref, uintptr_t) { return true; });
    pte_barrier();
    if (res.split) {
        flush_all();
    }
}

bool populate(range r, unsigned perm, size_t leaf_size, bool zero)
{
    if (anything_mapped(page_align(r))) {
        return false;
    }

    walk_opts o{true, true, false, level_of(leaf_size)};
    auto res = walk_range(page_align(r), o, [=](pte_ref e, uintptr_t) {
        if (!e.empty()) {
            return true;
        }
        size_t size = e.size();
        phys_addr p = frames::alloc(size, size);
        if (p == no_memory) {
            return false;
        }
        if (zero) {
            memset(frames::to_linear(p), 0, size);
        }
        pte expected = 0;
        if (!e.compare_exchange(expected, e.leaf_for(p, perm))) {
            // Another cpu mapped this address between the check above and here.
            // Just free the frame we got and leave the other mapping in place.
            frames::free(p, size);
        }
        return true;
    });
    pte_barrier();
    if (res.split) {
        flush_all();
    }
    return res.complete;
}

void depopulate(range r)
{
    pending_invalidation stale;
    struct {
        phys_addr addr;
        size_t size;
    } frame[flush_batch];
    unsigned held = 0;

    auto give_back = [&] {
        stale.epoch = flush_epoch();
        stale.invalidate();
        for (unsigned i = 0; i < held; ++i) {
            frames::free(frame[i].addr, frame[i].size);
        }
        held = 0;
    };

    walk_result res;
    clear_range(page_align(r), res, true, [&](uintptr_t va, phys_addr addr, size_t size) {
        if (held == flush_batch) {
            give_back();
        }
        stale.add(va);
        frame[held++] = {addr, size};
    });
    if (res.count) {
        stale.all = true;
    }
    give_back();
    res.settle();
}

}
}
