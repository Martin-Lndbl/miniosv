/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MMU_HH
#define MMU_HH

#include <stdint.h>
#include <osv/types.h>
#include <osv/error.h>
#include <osv/mmu-defs.hh>
#include <osv/align.hh>
#include <osv/trace.hh>
#include <osv/kernel_config.h>

struct exception_frame;

/**
 * MMU namespace
 */
namespace mmu {

// when we know it was dynamically allocated
inline phys virt_to_phys_dynamic_phys(void* virt)
{
    return static_cast<char*>(virt) - phys_mem;
}

inline unsigned pt_index(void *virt, unsigned level)
{
    return (reinterpret_cast<ulong>(virt) >> (page_size_shift + level * pte_per_page_shift)) & (pte_per_page - 1);
}

// Page-table work over a range: fill it with frames, empty it, change its
// permissions, split its huge leaves.
void populate_anon(void *region_start, void *addr, size_t size, unsigned perm,
                   bool write, bool small_pages, bool zero);
void depopulate_anon(void *region_start, void *addr, size_t size);
void protect_pages(void *region_start, void *addr, size_t size, unsigned perm);
void use_small_pages(void *region_start, void *addr, size_t size);

// Anonymous memory over a vspace reservation. There is no filesystem, so
// these are the only mappings.
void* map_anon(const void* addr, size_t size, unsigned flags, unsigned perm);

error munmap(const void* addr, size_t size);
error mprotect(const void *addr, size_t size, unsigned int perm);
error msync(const void* addr, size_t length, int flags);
error mincore(const void *addr, size_t length, unsigned char *vec);
bool is_linear_mapped(const void *addr, size_t size);
bool ismapped(const void *addr, size_t size);
bool isreadable(void *addr, size_t size);


static TRACEPOINT(trace_clear_pte, "ptep=%p, pte=%x", void*, uint64_t);

template<int N>
__attribute__((always_inline)) // Necessary because of issue #1029
inline pt_element<N> clear_pte(hw_ptep<N> ptep)
{
    auto old = ptep.exchange(make_empty_pte<N>());
    trace_clear_pte(ptep.release(), old.addr());
    return old;
}

template<int N>
inline pt_element<N> make_intermediate_pte(hw_ptep<N> ptep, phys addr)
{
    static_assert(pt_level_traits<N>::intermediate_capable::value, "level 0 pte cannot be intermediate");
    return make_pte<N>(addr, false);
}

template<int N>
inline pt_element<N> make_leaf_pte(hw_ptep<N> ptep, phys addr,
                                   unsigned perm = perm_rwx,
                                   mattr mem_attr = mattr_default)
{   
    static_assert(pt_level_traits<N>::leaf_capable::value, "non leaf pte");
    return make_pte<N>(addr, true, perm, mem_attr);
}

class virt_pte_visitor {
public:
    virtual void pte(pt_element<0>) = 0;
    virtual void pte(pt_element<1>) = 0;
};

void virt_visit_pte_rcu(uintptr_t virt, virt_pte_visitor& visitor);

template<int N>
inline bool write_pte(void *addr, hw_ptep<N> ptep, pt_element<N> old_pte, pt_element<N> new_pte)
{
    new_pte.mod_addr(virt_to_phys(addr));
    return ptep.compare_exchange(old_pte, new_pte);
}

template<int N>
inline bool write_pte(void *addr, hw_ptep<N> ptep, pt_element<N> pte)
{
    pte.mod_addr(virt_to_phys(addr));
    return ptep.compare_exchange(ptep.read(), pte);
}

// Linear-mapped memory is contiguous in both address spaces, so a range is
// always one physical run.
template <typename OutputFunc>
inline
void virt_to_phys(void* vaddr, size_t len, OutputFunc out)
{
    out(virt_to_phys(vaddr), len);
}

void* phys_to_virt(phys pa);

template <typename T>
T* phys_cast(phys pa)
{
    return static_cast<T*>(phys_to_virt(pa));
}

inline
bool is_page_aligned(intptr_t addr)
{
    return !(addr & (page_size-1));
}

inline
bool is_page_aligned(void* addr)
{
    return is_page_aligned(reinterpret_cast<intptr_t>(addr));
}

// The mattr type is defined differently for each architecture
// and interpreted by the architecture-specific code, and has
// an architecture-specific meaning.
// Currently mem_attr is ignored on x86_64. For aarch64 specifics see
// definitions in arch/aarch64/arch-mmu.hh
void linear_map(void* virt, phys addr, size_t size, const char* name,
                size_t slop = mmu::page_size,
                mattr mem_attr = mmu::mattr_default);

void free_initial_memory_range(uintptr_t addr, size_t size);
void switch_to_runtime_page_tables();

error  advise(void* addr, size_t size, int advice);

void vm_fault(uintptr_t addr, exception_frame* ef);

// Synchronize cpu data and instruction caches for specified area of virtual memory
void synchronize_cpu_caches(void *v, size_t size);
}

#endif /* MMU_HH */
