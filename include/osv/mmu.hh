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
#include <osv/kernel_config.h>

struct exception_frame;

/**
 * MMU namespace
 */
namespace mmu {

// when we know it was dynamically allocated
inline mem::frames::phys_addr virt_to_phys_dynamic_phys(void* virt)
{
    return static_cast<char*>(virt) - phys_mem;
}

inline unsigned pt_index(void *virt, unsigned level)
{
    return (reinterpret_cast<ulong>(virt) >> (page_size_shift + level * pte_per_page_shift)) & (pte_per_page - 1);
}

// Page-table work over a range: fill it with frames, empty it, change its
// permissions, split its huge leaves.
bool populate_anon(void *addr, size_t size, unsigned perm, bool small_pages, bool zero);
void depopulate_anon(void *addr, size_t size);
void protect_pages(void *addr, size_t size, unsigned perm);
void use_small_pages(void *addr, size_t size);

// Anonymous memory over a vspace reservation. There is no filesystem, so
// these are the only mappings.
void* map_anon(const void* addr, size_t size, unsigned flags, unsigned perm);

error munmap(const void* addr, size_t size);
error mprotect(const void *addr, size_t size, unsigned int perm);
error msync(const void* addr, size_t length, int flags);
bool is_linear_mapped(const void *addr, size_t size);
bool ismapped(const void *addr, size_t size);

void* phys_to_virt(mem::frames::phys_addr pa);

template <typename T>
T* phys_cast(mem::frames::phys_addr pa)
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

void linear_map(void* virt, mem::frames::phys_addr addr, size_t size, const char* name,
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
