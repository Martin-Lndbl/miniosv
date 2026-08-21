/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <sys/mman.h>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
#include <memory>
#include <osv/debug.hh>
#include "osv/trace.hh"
#include <osv/stubbing.hh>
#include "libc/libc.hh"
#include <safe-ptr.hh>
#include <atomic>
#include <osv/kernel_config.h>

#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0x4000000
#endif

TRACEPOINT(trace_memory_mmap, "addr=%p, length=%d, prot=%d, flags=%d, fd=%d, offset=%d", void *, size_t, int, int, int, off_t);
TRACEPOINT(trace_memory_mmap_err, "%d", int);
TRACEPOINT(trace_memory_mmap_ret, "%p", void *);
TRACEPOINT(trace_memory_munmap, "addr=%p, length=%d", void *, size_t);
TRACEPOINT(trace_memory_munmap_err, "%d", int);
TRACEPOINT(trace_memory_munmap_ret, "");

unsigned libc_prot_to_perm(int prot)
{
    unsigned perm = 0;
    if (prot & PROT_READ) {
        perm |= mem::perm_read;
    }
    if (prot & PROT_WRITE) {
        perm |= mem::perm_write;
    }
    if (prot & PROT_EXEC) {
        perm |= mem::perm_exec;
    }
    return perm;
}

static bool page_aligned(const void *p)
{
    return !(reinterpret_cast<uintptr_t>(p) & (mem::mapping::page_size - 1));
}

OSV_LIBC_API
int mprotect(void *addr, size_t len, int prot)
{
    // we don't support mprotecting() the linear map (e.g.., malloc() memory)
    // because that could leave the linear map a mess.
    if (reinterpret_cast<long>(addr) < 0) {
        abort("mprotect() on linear map not supported\n");
    }

    if (!page_aligned(addr)) {
        // address not page aligned
        return libc_error(EINVAL);
    }

    len = align_up(len, mem::mapping::page_size);
    if (!mem::heap::protect(addr, len, libc_prot_to_perm(prot))) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int mmap_validate(void *addr, size_t length, int flags, off_t offset)
{
    int type = flags & (MAP_SHARED|MAP_PRIVATE);
    // Either MAP_SHARED or MAP_PRIVATE must be set, but not both.
    if (!type || type == (MAP_SHARED|MAP_PRIVATE)) {
        return EINVAL;
    }
    if ((flags & MAP_FIXED && !page_aligned(addr)) ||
        !page_aligned(reinterpret_cast<void *>(offset)) || length == 0) {
        return EINVAL;
    }
    return 0;
}

OSV_LIBC_API
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset)
{
    trace_memory_mmap(addr, length, prot, flags, fd, offset);

    int err = mmap_validate(addr, length, flags, offset);
    if (err) {
        errno = err;
        trace_memory_mmap_err(err);
        return MAP_FAILED;
    }

    // make use the payload isn't remapping physical memory
    assert(reinterpret_cast<long>(addr) >= 0);

    void *ret;

    auto mmap_perm = libc_prot_to_perm(prot);

#ifndef AARCH64_PORT_STUB
    if ((flags & MAP_32BIT) && !(flags & MAP_FIXED) && !addr) {
        // If addr is not specified, OSv by default starts mappings at address
        // a low default. MAP_32BIT asks for a lower one still.
        // default. If MAP_FIXED or addr were specified, the default does not
        // matter anyway.
        addr = (void*)0x2000000ul;
    }
#endif
    // There is no filesystem, so only anonymous mappings are supported;
    // file-backed mmap is not available.
    if (!(flags & MAP_ANONYMOUS)) {
        errno = ENODEV;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    // Anonymous memory is heap memory: whole pages with a reservation of
    // their own. MAP_FIXED has no answer here, since the heap picks addresses.
    if (flags & MAP_FIXED) {
        errno = ENOTSUP;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    ret = mem::heap::alloc_pages(length, mmap_perm);
    if (!ret) {
        errno = ENOMEM;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    trace_memory_mmap_ret(ret);
    return ret;
}

int munmap_validate(void *addr, size_t length)
{
    if (!page_aligned(addr) || length == 0) {
        return EINVAL;
    }
    return 0;
}

OSV_LIBC_API
int munmap(void *addr, size_t length)
{
    trace_memory_munmap(addr, length);
    int error = munmap_validate(addr, length);
    if (error) {
        errno = error;
        trace_memory_munmap_err(error);
        return -1;
    }
    int ret = 0;
    if (mem::heap::owns(addr)) {
        mem::heap::free(addr);
    } else {
        errno = EINVAL;
        ret = -1;
        trace_memory_munmap_err(errno);
    }
    trace_memory_munmap_ret();
    return ret;
}

// Anonymous memory has no backing store, so this only reports whether the
// range is there at all.
OSV_LIBC_API
int msync(void *addr, size_t length, int flags)
{
    if (!mem::heap::owns(addr)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// Nothing is given back: the frames under an allocation belong to it until it
// is freed. MADV_DONTNEED is accepted and does nothing.
OSV_LIBC_API
int madvise(void *addr, size_t length, int advice)
{
    if (!mem::heap::owns(addr)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// brk/sbrk are not supported: nothing asks for them, and a program break
// wants a lazily backed region, which anonymous memory here is not.
OSV_LIBC_API
int brk(void *)
{
    errno = ENOMEM;
    return -1;
}

OSV_LIBC_API
void *sbrk(intptr_t)
{
    errno = ENOMEM;
    return (void *)-1;
}

OSV_LIBC_API
int posix_madvise(void *addr, size_t len, int advice) {
    return mem::heap::owns(addr) ? 0 : ENOMEM;
}
