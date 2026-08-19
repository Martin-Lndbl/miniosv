/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MMU_DEFS_HH
#define MMU_DEFS_HH

#include <stdint.h>
#include <osv/types.h>
#include <osv/mem/types.hh>
#include <osv/virt_to_phys.hh>

#include <mem.hh>

struct exception_frame;

namespace mmu {

constexpr uintptr_t page_size = 4096;
constexpr int page_size_shift = 12; // log2(page_size)

constexpr int pte_per_page = 512;
constexpr int pte_per_page_shift = 9; // log2(pte_per_page)

constexpr uintptr_t huge_page_size = mmu::page_size*pte_per_page; // 2 MB

typedef uint64_t f_offset;

enum class mem_area {
    main,
    page,
    mempool,
};

constexpr mem_area identity_mapped_areas[] = {
    mem_area::main,
    mem_area::page,
    mem_area::mempool,
};

constexpr uintptr_t mem_area_size = uintptr_t(1) << 44;

constexpr uintptr_t get_mem_area_base(mem_area area)
{
    return 0x400000000000 | uintptr_t(area) << 44;
}

static inline mem_area get_mem_area(void* addr)
{
    return mem_area(reinterpret_cast<uintptr_t>(addr) >> 44 & 3);
}

constexpr void* translate_mem_area(mem_area from, mem_area to, void* addr)
{
    return static_cast<void*>(static_cast<char*>(addr)
                              - get_mem_area_base(from) + get_mem_area_base(to));
}

constexpr uintptr_t main_mem_area_base = get_mem_area_base(mem_area::main);
static char* const phys_mem = reinterpret_cast<char*>(main_mem_area_base);

enum {
    perm_read = mem::perm_read,
    perm_write = mem::perm_write,
    perm_exec = mem::perm_exec,
    perm_rx = perm_read | perm_exec,
    perm_rw = perm_read | perm_write,
    perm_rwx = perm_read | perm_write | perm_exec,
};

enum {
    mmap_fixed       = 1ul << 0,
    mmap_populate    = 1ul << 1,
    mmap_uninitialized = 1ul << 3,
    mmap_small       = 1ul << 5,
};

enum {
    advise_dontneed = 1ul << 0,
    advise_nohugepage = 1ul << 1,
};

// The mattr type says how the hardware may cache and reorder accesses to a
// mapping. It is ignored on x86_64; for aarch64 specifics see the attribute
// indexes in arch/aarch64/mem/hw.hh and the mair_el1 setup in boot.S.
using mattr = mem::mattr;
constexpr mattr mattr_default = mattr::normal;

/* flush tlb for the current processor */
void flush_tlb_local();
/* flush tlb for all */
void flush_tlb_all();

constexpr size_t page_size_level(unsigned level)
{
    return size_t(1) << (page_size_shift + pte_per_page_shift * level);
}

/* take an error code coming from the exception frame, and return
   whether the error reports a page fault (insn/write) */
bool is_page_fault_insn(unsigned int err);
bool is_page_fault_write(unsigned int err);

bool fast_sigsegv_check(uintptr_t addr, exception_frame* ef);

}

#endif
