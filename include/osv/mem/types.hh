/*
 * Types shared by the memory layers.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_TYPES_HH
#define OSV_MEM_TYPES_HH

#include <stddef.h>
#include <stdint.h>

namespace mem {

// A half-open range of virtual addresses.
struct range {
    uintptr_t start = 0;
    uintptr_t end = 0;

    size_t size() const { return end - start; }
    bool empty() const { return end <= start; }
    bool contains(uintptr_t a) const { return a >= start && a < end; }
    bool contains(const range &r) const { return r.start >= start && r.end <= end; }
    bool intersects(const range &r) const { return r.start < end && start < r.end; }
    range clamp(const range &r) const {
        return {r.start > start ? r.start : start, r.end < end ? r.end : end};
    }
};

// Access permissions. The values match the hardware-facing mmu::perm_* set.
enum {
    perm_none = 0,
    perm_read = 1,
    perm_write = 2,
    perm_exec = 4,
    perm_rw = perm_read | perm_write,
    perm_rwx = perm_read | perm_write | perm_exec,
};

}

#endif
