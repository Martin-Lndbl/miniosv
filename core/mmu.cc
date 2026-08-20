/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mmu.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>
#include <osv/mempool.hh>
#include "processor.hh"
#include "exceptions.hh"
#include "dump.hh"
#include "libc/signal.hh"
#include <osv/debug.hh>
#include <string.h>
#include <assert.h>
#include <osv/align.hh>
#include <safe-ptr.hh>
#include <osv/error.h>
#include <osv/trace.hh>
#include <algorithm>

#include <osv/kernel_config.h>

extern void* elf_start;
extern size_t elf_size;

extern const char text_start[], text_end[];

namespace mmu {

// Physical base of the kernel ELF image and the runtime virtual->physical shift
// for it. Both are set during early arch setup from where the UEFI stub loaded
// the kernel (which is no longer a fixed physical address on either arch).
void *elf_phys_start;
extern "C" u64 kernel_vm_shift;

// The linear map belongs to the frame allocator; these are what the drivers
// and the boot code still call it by.
void* phys_to_virt(mem::frames::phys_addr pa)
{
    return mem::frames::to_linear(pa);
}

mem::frames::phys_addr virt_to_phys(void *virt)
{
    return mem::frames::from_linear(virt);
}

static mem::range page_range(const void *addr, size_t size)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    return {align_down(start, page_size), align_up(start + size, page_size)};
}

bool populate_anon(void *addr, size_t size, unsigned perm, bool small_pages, bool zero)
{
    if (!mem::mapping::populate(page_range(addr, size), perm,
                                small_pages ? page_size : huge_page_size, zero)) {
        return false;
    }
    // Nothing is done for the instruction cache here: these pages are freshly
    // allocated and hold no code yet, so there would be nothing to publish.
    // Whoever writes instructions into them owes that, and the page cache will
    // be the first thing in this kernel that does (step 7).
    return true;
}

void depopulate_anon(void *addr, size_t size)
{
    mem::mapping::depopulate(page_range(addr, size));
}

void protect_pages(void *addr, size_t size, unsigned perm)
{
    mem::mapping::protect(page_range(addr, size), perm);
}

void use_small_pages(void *addr, size_t size)
{
    mem::mapping::split(page_range(addr, size));
}

// Everything this file reserves. The kind tells a fault which of the two it
// landed in, and the rest is how an anonymous region wants its pages.
struct tracked_region {
    mem::vspace::region r;
    enum { anon, linear } kind;
    bool zero;
    bool small_pages;
};

static tracked_region *tracked_of(mem::vspace::region *r)
{
    return reinterpret_cast<tracked_region*>(r);
}

void* map_anon(const void* addr, size_t size, unsigned flags, unsigned perm)
{
    size = align_up(size, page_size);
    auto *t = new tracked_region();
    t->kind = tracked_region::anon;
    t->zero = !(flags & mmap_uninitialized);
    t->small_pages = flags & mmap_small;
    t->r.perm = perm;

    auto start = reinterpret_cast<uintptr_t>(addr);
    auto result = (flags & mmap_fixed)
        ? mem::vspace::reserve_at(t->r, {start, start + size})
        : mem::vspace::reserve(t->r, size, size >= huge_page_size ? huge_page_size : page_size);
    if (result != mem::vspace::resa_result::success && !(flags & mmap_fixed) &&
        size >= huge_page_size) {
        // Huge alignment is a preference: a mapping without it beats no mapping.
        result = mem::vspace::reserve(t->r, size, page_size);
    }
    if (result != mem::vspace::resa_result::success) {
        delete t;
        throw make_error(ENOMEM);
    }

    void *v = reinterpret_cast<void*>(t->r.span.start);
    if ((flags & mmap_populate) &&
        !populate_anon(v, size, perm, t->small_pages, t->zero)) {
        depopulate_anon(v, size);
        mem::vspace::release(t->r);
        delete t;
        throw make_error(ENOMEM);
    }
    return v;
}

// The region starting exactly here, or null.
static tracked_region *anon_at(const void *addr, size_t size)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    auto *r = mem::vspace::lookup(start);
    if (!r || r->span.start != start || r->span.size() != align_up(size, page_size)) {
        return nullptr;
    }
    auto *t = tracked_of(r);
    return t->kind == tracked_region::anon ? t : nullptr;
}

bool is_linear_mapped(const void *addr, size_t size)
{
    if ((addr >= elf_start) && (static_cast<const char*>(addr) + size <= static_cast<char*>(elf_start) + elf_size)) {
        return true;
    }
    return addr >= phys_mem;
}

// Is every byte of this region reserved in the address space?
bool ismapped(const void *addr, size_t size)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    return mem::vspace::reserved({start, start + size});
}

void linear_map(void* _virt, mem::frames::phys_addr addr, size_t size, const char* name,
                size_t slop, mattr mem_attr)
{
    uintptr_t virt = reinterpret_cast<uintptr_t>(_virt);
    // Rounding the range outward is only safe as far as the two addresses run
    // together, and never further than the largest leaf.
    slop = std::min(slop, huge_page_size);
    assert((virt & (slop - 1)) == (addr & (slop - 1)));
    // The linear map is built from firmware ranges that share leaves, and some
    // of them are mapped again later by drivers reaching for a table inside
    // one, so overlapping is normal here and only a disagreement is an error.
    if (!mem::mapping::attach_missing({virt, virt + size}, addr, perm_rwx, slop,
                                      mem_attr)) {
        abort("linear_map: %p is already mapped to other physical memory\n", _virt);
    }

    // Hold the range so nothing else is handed the same addresses. Some of
    // these overlap -- ACPI maps pages that are also part of a reserved range
    // -- and the first reservation is enough to keep them all out.
    auto *t = new tracked_region();
    t->kind = tracked_region::linear;
    t->r.perm = perm_rwx;
    if (mem::vspace::reserve_at(t->r, {virt, virt + size}) !=
        mem::vspace::resa_result::success) {
        delete t;
    }
}

void free_initial_memory_range(uintptr_t addr, size_t size)
{
    if (!size) {
        return;
    }
    // Most of the time the kernel code references memory using
    // virtual addresses. However some allocated system structures
    // like page tables use physical addresses.
    // For that reason we skip the very 1st page of physical memory
    // so that allocated memory areas NEVER map to physical address 0.
    if (!addr) {
        ++addr;
        --size;
    }
    memory::free_initial_memory_range(phys_cast<void>(addr), size);
}

// Permissions live in the page tables; the region records what was asked for.
error mprotect(const void *addr, size_t len, unsigned perm)
{
    len = align_up(len, page_size);
    auto start = reinterpret_cast<uintptr_t>(addr);
    if (!ismapped(addr, len)) {
        return make_error(ENOMEM);
    }
    if (auto *r = mem::vspace::lookup(start)) {
        r->perm = perm;
    }
    protect_pages(const_cast<void*>(addr), len, perm);
    return no_error();
}

error munmap(const void *addr, size_t length)
{
    auto *t = anon_at(addr, length);
    if (!t) {
        return make_error(EINVAL);
    }
    depopulate_anon(const_cast<void*>(addr), t->r.span.size());
    mem::vspace::release(t->r);
    delete t;
    return no_error();
}

error advise(void* addr, size_t size, int advice)
{
    size = align_up(size, page_size);
    if (!ismapped(addr, size)) {
        return make_error(ENOMEM);
    }
    if (advice == advise_dontneed) {
        depopulate_anon(addr, size);
        return no_error();
    }
    if (advice == advise_nohugepage) {
        use_small_pages(addr, size);
        return no_error();
    }
    return make_error(EINVAL);
}

// There is nowhere to write anonymous memory back to, so this only reports
// whether the range is mapped at all.
error msync(const void* addr, size_t length, int flags)
{
    return ismapped(addr, length) ? no_error() : make_error(ENOMEM);
}

TRACEPOINT(trace_mmu_vm_fault, "addr=%p, error_code=%x", uintptr_t, unsigned int);
TRACEPOINT(trace_mmu_vm_fault_sigsegv, "addr=%p, error_code=%x, %s", uintptr_t, unsigned int, const char*);
TRACEPOINT(trace_mmu_vm_fault_ret, "addr=%p, error_code=%x", uintptr_t, unsigned int);

static void vm_sigsegv(uintptr_t addr, exception_frame* ef)
{
    void *pc = ef->get_pc();
    if (pc >= text_start && pc < text_end) {
        debug_ll("page fault outside application, addr: 0x%016lx\n", addr);
        dump_registers(ef);
        abort();
    }
    osv::handle_mmap_fault(addr, SIGSEGV, ef);
}

static bool permitted(unsigned perm, unsigned error_code)
{
    if (mem::mapping::is_page_fault_insn(error_code)) {
        return perm & perm_exec;
    }
    if (mem::mapping::is_page_fault_write(error_code)) {
        return perm & perm_write;
    }
    return perm & perm_read;
}

// Anonymous memory is the only thing that faults, so a fault is a region
// lookup and the pages that region wants.
void vm_fault(uintptr_t addr, exception_frame* ef)
{
    unsigned error = ef->get_error();
    trace_mmu_vm_fault(addr, error);
    if (mem::mapping::fast_sigsegv_check(addr, ef)) {
        vm_sigsegv(addr, ef);
        trace_mmu_vm_fault_sigsegv(addr, error, "fast");
        return;
    }
    addr = align_down(addr, page_size);

    auto *r = mem::vspace::lookup(addr);
    auto *t = r ? tracked_of(r) : nullptr;
    if (!t || t->kind != tracked_region::anon || !permitted(r->perm, error)) {
        vm_sigsegv(addr, ef);
        trace_mmu_vm_fault_sigsegv(addr, error, "slow");
        return;
    }

    size_t size = page_size;
    if (!t->small_pages) {
        uintptr_t huge_start = align_up(r->span.start, huge_page_size);
        uintptr_t huge_end = align_down(r->span.end, huge_page_size);
        if (huge_start <= addr && addr < huge_end) {
            addr = align_down(addr, huge_page_size);
            size = huge_page_size;
        }
    }
    // populate() refuses a range that is already mapped, which is what a
    // second thread faulting the same leaf sees once the first one has been
    // through. The fault is satisfied either way; only an address still
    // without a translation is a failure.
    if (!populate_anon(reinterpret_cast<void*>(addr), size, r->perm,
                       t->small_pages, t->zero) &&
        !mem::mapping::find(addr)) {
        vm_sigsegv(addr, ef);
        trace_mmu_vm_fault_sigsegv(addr, error, "nomem");
        return;
    }
    trace_mmu_vm_fault_ret(addr, error);
}
}

extern "C" bool is_linear_mapped(const void *addr)
{
    return addr >= mmu::phys_mem;
}
