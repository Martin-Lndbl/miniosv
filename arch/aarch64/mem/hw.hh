/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * aarch64 specific memory operations and formats.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_MEM_HW_HH
#define ARCH_MEM_HW_HH

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <osv/types.h>
#include <osv/mem/types.hh>

namespace mem {
namespace mapping {

using pte = uint64_t;
constexpr unsigned levels = 4;
constexpr unsigned entries_per_table = 512;

constexpr unsigned max_leaf_level = 1; // only 2MiB for now

// Software available bits, starting at bit 56.
constexpr unsigned sw_bits = 3;

constexpr unsigned level_shift(unsigned level)
{
    return 12 + 9 * level;
}

constexpr size_t level_size(unsigned level)
{
    return size_t(1) << level_shift(level);
}

constexpr unsigned level_index(uintptr_t va, unsigned level)
{
    return (va >> level_shift(level)) & (entries_per_table - 1);
}

enum : pte {
    pte_valid = pte(1) << 0,   // descriptor bits [1:0]: 0b01 block, 0b11 table or page
    pte_table = pte(1) << 1,
    pte_ap2   = pte(1) << 7,   // set means read-only
    pte_sh    = pte(3) << 8,   // inner shareable
    pte_af    = pte(1) << 10,  // access flag
    pte_pxn   = pte(1) << 53,  // privileged execute never
    pte_d     = pte(1) << 55,  // dirty, maintained by software
};

constexpr unsigned addr_bits = 48;

constexpr pte attr_index(unsigned idx) { return pte(idx) << 2; }

// Indexes into mair_el1, which boot.S programs.
constexpr unsigned attr_normal = 4;
constexpr unsigned attr_device = 0;

constexpr pte addr_mask(bool large)
{
    return ((pte(1) << addr_bits) - 1) & ~pte(large ? 0x1fffff : 0xfff);
}

inline bool pte_empty(pte e) { return !e; }
inline bool pte_present(pte e) { return e & pte_valid; }

inline bool pte_is_leaf(pte e, unsigned level)
{
    return level == 0 || (level <= max_leaf_level && !(e & pte_table));
}

inline frames::phys_addr pte_addr(pte e, unsigned level)
{
    return e & addr_mask(level > 0 && pte_is_leaf(e, level));
}

inline frames::phys_addr pte_table_addr(pte e) { return e & addr_mask(false); }

inline unsigned pte_perm(pte e)
{
    if (!(e & pte_valid)) {
        return 0;
    }
    return perm_read | ((e & pte_ap2) ? 0 : perm_write) |
           ((e & pte_pxn) ? 0 : perm_exec);
}

inline pte pte_make_table(frames::phys_addr p)
{
    return p | pte_valid | pte_table;
}

inline pte pte_make_leaf(frames::phys_addr p, unsigned perm, unsigned level, mattr ma)
{
    pte e = p | pte_af | pte_d | pte_sh |
            attr_index(ma == mattr::dev ? attr_device : attr_normal);
    if (level == 0) {
        e |= pte_table;
    }
    if (perm) {
        e |= pte_valid;
    }
    if (!(perm & perm_write)) {
        e |= pte_ap2;
    }
    if (!(perm & perm_exec)) {
        e |= pte_pxn;
    }
    return e;
}

// Change permissions on an existing entry.
// Possible: none, read, read+write, read+exec, read+write+exec.
inline pte pte_with_perm(pte e, unsigned perm)
{
    e = perm ? e | pte_valid : e & ~pte_valid;
    e = (perm & perm_write) ? e & ~pte_ap2 : e | pte_ap2;
    return (perm & perm_exec) ? e & ~pte_pxn : e | pte_pxn;
}

// Every permission change needs a flush.
inline bool pte_perm_change_needs_flush(unsigned old, unsigned neu)
{
    return old != neu;
}

constexpr bool tracks_writes = false;

inline pte pte_set_present(pte e, bool v) { return v ? e | pte_valid : e & ~pte_valid; }
inline bool pte_accessed(pte e) { return e & pte_af; }
inline bool pte_dirty(pte e) { return e & pte_d; }
inline pte pte_set_accessed(pte e, bool v) { return v ? e | pte_af : e & ~pte_af; }
inline pte pte_set_dirty(pte e, bool v) { return v ? e | pte_d : e & ~pte_d; }

inline bool pte_sw_bit(pte e, unsigned n) { return (e >> (56 + n)) & 1; }

inline pte pte_set_sw_bit(pte e, unsigned n, bool v)
{
    pte bit = pte(1) << (56 + n);
    return v ? e | bit : e & ~bit;
}

// The entry one level down that maps the `index`th slice of what `e` maps at
// `level`. Only the address and the descriptor kind change; the attributes,
// the permissions and the software bits are the same memory's.
inline pte pte_demote(pte e, unsigned level, unsigned index)
{
    frames::phys_addr a = pte_addr(e, level) + (frames::phys_addr(index) << level_shift(level - 1));
    pte flags = e & ~addr_mask(false) & ~pte_table;
    if (level - 1 == 0) {
        flags |= pte_table;
    }
    return flags | a;
}

// aarch64 needs barriers to ensure writes are visible.
inline void pte_barrier()
{
    asm volatile("dsb ishst; isb" ::: "memory");
}

// aarch64 has the tlbi instruction, which can broadcast to all cores, so we don't need to send IPIs.
constexpr bool tlb_is_broadcast = true;

}
}

namespace mem {
namespace frames {

// Base of physical RAM, rounded down to 2 MiB. Set by uefi_memory_setup().
extern u64 ram_base;

}
}

#endif /* ARCH_MEM_HW_HH */
