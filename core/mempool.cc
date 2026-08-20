/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mem/early.hh>
#include <osv/mem/heap.hh>
#include <osv/mempool.hh>
#include <osv/ilog2.hh>
#include "arch-setup.hh"
#include <cassert>
#include <cstdint>
#include <new>
#include <string.h>
#include "libc/libc.hh"
#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/kernel_config.h>
#if CONF_memory_tracker
#include <osv/alloctracker.hh>
#endif
#include <atomic>
#include <osv/mmu.hh>
#include <osv/mem/frames.hh>
#include "mem/linear.hh"
#include <osv/trace.hh>
#include <osv/preempt-lock.hh>
#include <osv/sched.hh>
#include <algorithm>
#include <osv/prio.hh>
#include <stdlib.h>
#include <osv/defer.hh>
#include <osv/migration-lock.hh>
#include <osv/export.h>

#include <osv/kernel_config.h>

#include <boost/dynamic_bitset.hpp>
#include <boost/lockfree/stack.hpp>
#include <boost/lockfree/policies.hpp>

TRACEPOINT(trace_memory_malloc, "buf=%p, len=%d, align=%d", void *, size_t,
           size_t);
TRACEPOINT(trace_memory_malloc_mempool, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_malloc_large, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_malloc_page, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_free, "buf=%p", void *);
TRACEPOINT(trace_memory_realloc, "in=%p, newlen=%d, out=%p", void *, size_t, void *);
TRACEPOINT(trace_memory_page_alloc, "page=%p", void*);
TRACEPOINT(trace_memory_page_free, "page=%p", void*);

std::atomic<unsigned int> smp_allocator_cnt{};
bool smp_allocator = false;
namespace memory {

size_t phys_mem_size;

#if CONF_memory_tracker
// Optionally track living allocations, and the call chain which led to each
// allocation. Don't set tracker_enabled before tracker is fully constructed.
alloc_tracker tracker;
bool tracker_enabled = false;
static inline void tracker_remember(void *addr, size_t size)
{
    // Check if tracker_enabled is true, but expect (be quicker in the case)
    // that it is false.
    if (__builtin_expect(tracker_enabled, false)) {
        tracker.remember(addr, size);
    }
}
static inline void tracker_forget(void *addr)
{
    if (__builtin_expect(tracker_enabled, false)) {
        tracker.forget(addr);
    }
}
#endif

// Before smp_allocator, threads are not yet available: malloc and free are
// used as soon as virtual memory is up, and sched::cpu::current() reads a TLS
// slot that is only set later.

// Hand the boot regions over to llfree. Still single-threaded here, which is
// what that hand-over needs.
struct start_frame_allocator {
    start_frame_allocator() { mem::frames::init(sched::cpus.size()); }
} s_start_frame_allocator __attribute__((init_priority((int)init_prio::frame_allocator)));

// The heap may only be used once every cpu is running.
static sched::cpu::notifier smp_allocator_notifier([] () {
    if (++smp_allocator_cnt == sched::cpus.size()) {
        mem::frames::enable_percpu();
        // The heap wants a per-cpu bump pointer and a reservation to fill, so
        // it can only take over once every cpu is up and the layers under it
        // are running. Everything before this came from the early allocator.
        mem::heap::init();
        smp_allocator = true;
    }
});

page_range::page_range(size_t _size)
    : size(_size)
{
}

struct addr_cmp {
    bool operator()(const page_range& fpr1, const page_range& fpr2) const {
        return &fpr1 < &fpr2;
    }
};

namespace bi = boost::intrusive;


// Our notion of free memory is "whatever is in the page ranges". Therefore it
// starts at 0, and increases as we add page ranges.
//
// There is nothing to reclaim: no page cache, no shrinkers, and the one client
// that can give memory back registers with frames::watch_pressure() long before
// it gets this far. So say what was asked for and stop, rather than block on a
// wait that nobody will ever satisfy.
void oom(size_t bytes)
{
    abort("Out of memory: %zu bytes requested, %zu MiB free of %zu MiB.\n",
          bytes, mem::frames::free_bytes() >> 20, mem::frames::total_available_bytes() >> 20);
}





// Physically contiguous memory with its size in a header. What is left of the
// old large allocator: the heap serves everything malloc asks for that does
// not have to be contiguous, so this is reached only through
// alloc_phys_contiguous_aligned() and by the alignments the heap refuses.
static void* malloc_large(size_t size, size_t alignment)
{
    auto requested_size = size;
    size_t offset;
    if (alignment < page_size) {
        offset = align_up(sizeof(page_range), alignment);
    } else {
        offset = page_size;
    }
    size += offset;
    size = align_up(size, page_size);

    // The header sits at the start of the allocation and the payload one
    // `offset` above it, so the payload is only as aligned as the base. Coarser
    // alignments went through alloc_phys_contiguous_aligned(), which has no
    // header; nothing in the tree asks malloc() for them.
    if (alignment > page_size) {
        abort("malloc: alignment %zu above the page size is not supported\n", alignment);
    }

    // Contiguous physical memory, with the size recorded in a header so that
    // free() can give back exactly what was taken.
    mem::frames::phys_addr p = mem::frames::alloc(size, page_size);
    void* mem = p ? mem::frames::to_linear(p) : nullptr;
    if (mem) {
        auto ret_header = new (mem) page_range(size);
        void* obj = reinterpret_cast<char*>(ret_header) + offset;
        trace_memory_malloc_large(obj, requested_size, size, alignment);
        return obj;
    }
    return nullptr;
}


static void free_large(void* obj)
{
    obj = align_down(static_cast<char*>(obj) - 1, page_size);
    mem::frames::free(mem::frames::from_linear(obj), static_cast<page_range*>(obj)->size);
}

static size_t large_object_offset(void *&obj)
{
    void *original_obj = obj;
    obj = align_down(static_cast<char*>(obj) - 1, page_size);
    return reinterpret_cast<uint64_t>(original_obj) - reinterpret_cast<uint64_t>(obj);
}

static size_t large_object_size(void *obj)
{
    size_t offset = large_object_offset(obj);
    auto header = static_cast<page_range*>(obj);
    return header->size - offset;
}


static void* untracked_alloc_page()
{
    void* ret = mem::frames::to_linear(mem::frames::alloc());
    if (!ret) {
        oom(page_size);
    }
    trace_memory_page_alloc(ret);
    return ret;
}

void* alloc_page()
{
    void *p = untracked_alloc_page();
#if CONF_memory_tracker
    tracker_remember(p, page_size);
#endif
    return p;
}

static inline void untracked_free_page(void *v)
{
    trace_memory_page_free(v);
    mem::frames::free(mem::frames::from_linear(v));
}

void free_page(void* v)
{
    untracked_free_page(v);
#if CONF_memory_tracker
    tracker_forget(v);
#endif
}

/* Allocate a huge page of a given size N (which must be a power of two)
 * N bytes of contiguous physical memory whose address is a multiple of N.
 * Memory allocated with alloc_huge_page() must be freed with free_huge_page(),
 * not free(), as the memory is not preceded by a header.
 */
void* alloc_huge_page(size_t N)
{
    return mem::frames::to_linear(mem::frames::alloc(N, N));
}

void free_huge_page(void* v, size_t N)
{
    mem::frames::free(mem::frames::from_linear(v), N);
}

void free_initial_memory_range(mem::frames::phys_addr addr, size_t size)
{
    mem::frames::add_region(addr, size);
}

void  __attribute__((constructor(init_prio::mempool))) setup()
{
    arch_setup_free_memory();
}

}

extern "C" {
    void* malloc(size_t size);
    void free(void* object);
    size_t malloc_usable_size(void *object);
}


/*
 * Where allocation sizes fall, and how many frees arrive knowing the size.
 *
 * The heap that replaces this one wants the size at free. C++ supplies it at
 * every delete of a known type and libc++ throws it away; C's free(ptr) never
 * has it. What the split actually looks like under a real workload decides
 * whether the new heap can recover a size from an address, or must be told.
 *
 * Counts are per-cpu and unsynchronised: a thread migrating mid-increment can
 * lose one, which does not matter for a distribution.
 */
#if CONF_memory_histogram
namespace memory {

constexpr unsigned hist_buckets = 40;

struct alignas(64) hist_row {
    uint64_t alloc[hist_buckets];
    uint64_t freed_sized[hist_buckets];
    uint64_t freed_total;
};

static hist_row hist[sched::max_cpus];

static inline hist_row &hist_row_for()
{
    if (!smp_allocator) {
        return hist[0];
    }
    auto *c = sched::cpu::current();
    return hist[c && c->id < sched::max_cpus ? c->id : 0];
}

static inline unsigned hist_bucket(size_t n)
{
    unsigned b = n < 2 ? 0 : 63 - __builtin_clzll(n);
    return b < hist_buckets ? b : hist_buckets - 1;
}

static inline void hist_alloc(size_t n)
{
    hist_row_for().alloc[hist_bucket(n)]++;
}

static inline void hist_freed_sized(size_t n)
{
    hist_row_for().freed_sized[hist_bucket(n)]++;
}

// Every free lands here, including the sized deletes below, which fall
// through to free() once they have recorded their size.
static inline void hist_freed()
{
    hist_row_for().freed_total++;
}

void histogram_dump()
{
    // Every exit route ends in poweroff(), and some of them arrive twice.
    static std::atomic<bool> dumped;
    if (dumped.exchange(true)) {
        return;
    }

    uint64_t alloc[hist_buckets] = {}, sized[hist_buckets] = {}, freed = 0;
    for (unsigned c = 0; c < sched::max_cpus; c++) {
        for (unsigned b = 0; b < hist_buckets; b++) {
            alloc[b] += hist[c].alloc[b];
            sized[b] += hist[c].freed_sized[b];
        }
        freed += hist[c].freed_total;
    }

    uint64_t alloc_total = 0, sized_total = 0;
    for (unsigned b = 0; b < hist_buckets; b++) {
        alloc_total += alloc[b];
        sized_total += sized[b];
    }
    if (!alloc_total) {
        return;
    }

    printf("\n######## allocation histogram ########\n");
    printf("%14s %14s %8s %14s\n", "size", "allocations", "share", "sized frees");
    uint64_t cum = 0;
    for (unsigned b = 0; b < hist_buckets; b++) {
        if (!alloc[b] && !sized[b]) {
            continue;
        }
        cum += alloc[b];
        char label[24];
        uint64_t lo = b ? (uint64_t(1) << b) : 0;
        if (lo >= (1ul << 20)) {
            snprintf(label, sizeof(label), "%lu MiB", lo >> 20);
        } else if (lo >= (1ul << 10)) {
            snprintf(label, sizeof(label), "%lu KiB", lo >> 10);
        } else {
            snprintf(label, sizeof(label), "%lu B", lo);
        }
        printf("%12s.. %14lu %7.2f%% %14lu\n", label, alloc[b],
               100.0 * cum / alloc_total, sized[b]);
    }
    printf("\n  allocations     %lu\n", alloc_total);
    printf("  frees           %lu\n", freed);
    printf("  of them sized   %lu (%.2f%%)\n", sized_total,
           100.0 * sized_total / (freed ? freed : 1));
    printf("######## end allocation histogram ########\n");
    fflush(stdout);
}

}
#else
namespace memory {
static inline void hist_alloc(size_t) {}
static inline void hist_freed_sized(size_t) {}
static inline void hist_freed() {}
void histogram_dump() {}
}
#endif

static inline void* std_malloc(size_t size, size_t alignment)
{
    if ((ssize_t)size < 0)
        return libc_error_ptr<void *>(ENOMEM);
    void *ret;
    if (mem::heap::ready() && mem::heap::takes(size, alignment)) {
        ret = mem::heap::alloc(size, alignment);
        trace_memory_malloc_mempool(ret, size, ret ? mem::heap::size_of(ret) : 0,
                                    alignment);
    } else if (!smp_allocator && mem::early::takes(size, alignment)) {
        ret = mem::early::alloc(size, alignment);
    } else {
        ret = memory::malloc_large(size, alignment);
    }
#if CONF_memory_tracker
    memory::tracker_remember(ret, size);
#endif
    memory::hist_alloc(size);
    return ret;
}

void* calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return malloc(0);
    if (nmemb > std::numeric_limits<size_t>::max() / size)
        return nullptr;
    auto n = nmemb * size;
    auto p = malloc(n);
    if (!p)
        return nullptr;
    memset(p, 0, n);
    return p;
}

static size_t object_size(void *object)
{
    if (mem::heap::owns(object)) {
        return mem::heap::size_of(object);
    }
    // Anything else came from before the heap existed, or from the contiguous
    // allocator, and both of those live in the linear map.
    assert(mem::frames::in_linear_map(object, 0));
    if (mem::early::owns(object)) {
        return mem::early::size_of(object);
    }
    return memory::large_object_size(object);
}

static inline void* std_realloc(void* object, size_t size)
{
    if (!object)
        return malloc(size);
    if (!size) {
        free(object);
        return nullptr;
    }

    size_t old_size = object_size(object);
    size_t copy_size = size > old_size ? old_size : size;
    void* ptr = malloc(size);
    if (ptr) {
        memcpy(ptr, object, copy_size);
        free(object);
    }

    return ptr;
}

// Everything free() does before it decides who the object goes back to.
// False if there is nothing to give back.
static inline bool free_bookkeeping(void *object)
{
    trace_memory_free(object);
    if (!object) {
        return false;
    }
    memory::hist_freed();
#if CONF_memory_tracker
    memory::tracker_forget(object);
#endif
    return true;
}

// Where a pointer the heap does not own goes back to: the early allocator, a
// whole page, or the contiguous allocator. All of them are in the linear map,
// which is what the alias in the address names.
static void free_foreign(void *object)
{
    assert(mem::frames::in_linear_map(object, 0));
    if (mem::early::owns(object)) {
        return mem::early::free(object);
    }
    return memory::free_large(object);
}

void free(void* object)
{
    if (!free_bookkeeping(object)) {
        return;
    }
    if (mem::heap::owns(object)) {
        return mem::heap::free(object);
    }
    free_foreign(object);
}

// The same with the size the caller kept, which is what operator delete has.
static inline void free_sized(void *object, size_t bytes)
{
    if (!free_bookkeeping(object)) {
        return;
    }
    memory::hist_freed_sized(bytes);
    if (mem::heap::owns(object)) {
        return mem::heap::free(object, bytes);
    }
    // Not the heap's, and nothing else here can use a size.
    free_foreign(object);
}

void* malloc(size_t size)
{
    static_assert(alignof(max_align_t) >= 2 * sizeof(size_t),
                  "alignof(max_align_t) smaller than glibc alignment guarantee");
    auto alignment = alignof(max_align_t);
    if (alignment > size) {
        alignment = 1ul << ilog2_roundup(size);
    }
    void* buf = std_malloc(size, alignment);

    trace_memory_malloc(buf, size, alignment);
    return buf;
}

OSV_LIBC_API
void* realloc(void* obj, size_t size)
{
    void* buf = std_realloc(obj, size);
    trace_memory_realloc(obj, size, buf);
    return buf;
}

extern "C" OSV_LIBC_API
void *reallocarray(void *ptr, size_t nmemb, size_t elem_size)
{
    size_t bytes;
    if (__builtin_mul_overflow(nmemb, elem_size, &bytes)) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(ptr, nmemb * elem_size);
}

OSV_LIBC_API
size_t malloc_usable_size(void* obj)
{
    if ( obj == nullptr ) {
        return 0;
    }
    return object_size(obj);
}

// posix_memalign() and C11's aligned_alloc() return an aligned memory block
// that can be freed with an ordinary free().

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    // posix_memalign() but not aligned_alloc() adds an additional requirement
    // that alignment is a multiple of sizeof(void*). We don't verify this
    // requirement, and rather always return memory which is aligned at least
    // to sizeof(void*), even if lesser alignment is requested.
    if (!is_power_of_two(alignment)) {
        return EINVAL;
    }
    void* ret = std_malloc(size, alignment);
    trace_memory_malloc(ret, size, alignment);
    if (!ret) {
        return ENOMEM;
    }
    // Until we have a full implementation, just croak if we didn't get
    // the desired alignment.
    assert (!(reinterpret_cast<uintptr_t>(ret) & (alignment - 1)));
    *memptr = ret;
    return 0;

}

void *aligned_alloc(size_t alignment, size_t size)
{
    void *ret;
    int error = posix_memalign(&ret, alignment, size);
    if (error) {
        errno = error;
        return NULL;
    }
    return ret;
}

// memalign() is an older variant of aligned_alloc(), which does not require
// that size be a multiple of alignment.
// memalign() is considered to be an obsolete SunOS-ism, but Linux's glibc
// supports it, and some applications still use it.
OSV_LIBC_API
void *memalign(size_t alignment, size_t size)
{
    return aligned_alloc(alignment, size);
}

namespace memory {

// Straight to the frame allocator: the caller passes the size back at free
// time, so there is no header, and therefore no need for the allocation to be
// offset to keep a header out of the payload's way.
void* alloc_phys_contiguous_aligned(size_t size, size_t align, bool block)
{
    assert(is_power_of_two(align));
    auto p = mem::frames::alloc(size, align);
    if (!p) {
        return nullptr;
    }
    void* ret = mem::frames::to_linear(p);
    assert(!(reinterpret_cast<uintptr_t>(ret) & (align - 1)));
    return ret;
}

void free_phys_contiguous_aligned(void* p, size_t size)
{
    mem::frames::free(mem::frames::from_linear(p), size);
}

}

extern "C" void* alloc_contiguous_aligned(size_t size, size_t align)
{
    return memory::alloc_phys_contiguous_aligned(size, align, true);
}

extern "C" void free_contiguous_aligned(void* p, size_t size)
{
    memory::free_phys_contiguous_aligned(p, size);
}

/*
 * The sized forms of operator delete. libc++ defines these weakly as a plain
 * free(), which drops the one thing the compiler went to the trouble of
 * supplying: the compiler emits the size at every delete of a known type, and
 * the histogram says that is 99.6% of the frees two real applications make.
 * Defining them here keeps it and hands it to the heap.
 */
#include <new>

void operator delete(void *p, size_t n) noexcept
{
    free_sized(p, n);
}

void operator delete[](void *p, size_t n) noexcept
{
    free_sized(p, n);
}

void operator delete(void *p, size_t n, std::align_val_t) noexcept
{
    free_sized(p, n);
}

void operator delete[](void *p, size_t n, std::align_val_t) noexcept
{
    free_sized(p, n);
}
