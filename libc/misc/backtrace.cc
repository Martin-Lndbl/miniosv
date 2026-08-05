/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/execinfo.h>
#include <osv/execinfo.hh>

#include <setjmp.h>
#include <unwind.h>

#include <atomic>

struct worker_arg {
    void **buffer;
    unsigned long *cfas;  // optional out-array for canonical frame addresses
    int size;
    int pos;
    unsigned long prevcfa;
};

static _Unwind_Reason_Code worker (struct _Unwind_Context *ctx, void *data)
{
    struct worker_arg *arg = (struct worker_arg *) data;
    if (arg->pos >= 0) {
        arg->buffer[arg->pos] = (void *)_Unwind_GetIP(ctx);
        unsigned long cfa = _Unwind_GetCFA(ctx);
        if (arg->cfas) {
            arg->cfas[arg->pos] = cfa;
        }
        if (arg->pos > 0 && arg->buffer[arg->pos-1] == arg->buffer[arg->pos]
                         && arg->prevcfa == cfa) {
            return _URC_END_OF_STACK;
        }
        arg->prevcfa = cfa;
    }
    if (++arg->pos == arg->size) {
        return _URC_END_OF_STACK;
    }
    return _URC_NO_REASON;
}

// Unwinding runs in contexts nobody chose: a tracepoint backtrace, the
// alloctracker and a panic raised inside a handler all unwind from an
// interrupt, on top of whatever instruction the CPU happened to be executing.
// At an arbitrary instruction the CFI is not always a description of a
// complete frame - half-built prologues, half-torn-down epilogues and
// hand-written assembly all occur - and libunwind will happily compute a
// canonical frame address from it and dereference it. Backtracing must not be
// able to kill the kernel, so the fault handlers hand a faulting unwind back
// here (see recover_from_fault below) and the walk returns the frames it had
// managed to collect. That is what the frame-pointer walker this replaced did
// with its hand-written safe loads.
//
// Per-thread rather than per-CPU: an interrupt handler unwinds in the context
// of the thread it interrupted, and a thread can migrate mid-unwind.
static __thread bool unwinding;
static __thread jmp_buf unwind_recovery;

// The fault handler diverts the faulting instruction here. It runs in the
// faulting context, with the exception frame already popped, and leaves it the
// only way that context can be left safely.
extern "C" [[noreturn]] void unwind_fault_landing_pad()
{
    longjmp(unwind_recovery, 1);
}

static std::atomic<unsigned long> unwind_fault_count;

// Called from the arch page-fault handlers with the pc they are about to
// resume at. Returns true (and replaces it) if this fault is an unwind walking
// off into nothing, rather than a fault the kernel should be handling.
bool osv::unwind_recover_from_fault(void **pc)
{
    if (!unwinding) {
        return false;
    }
    unwind_fault_count.fetch_add(1, std::memory_order_relaxed);
    *pc = (void *)unwind_fault_landing_pad;
    return true;
}

unsigned long osv::unwind_faults()
{
    return unwind_fault_count.load(std::memory_order_relaxed);
}

void osv::unwind_abandon()
{
    unwinding = false;
}

// pos starts at -1 so the callback skips the frame that ran _Unwind_Backtrace
// and the returned buffer starts at its caller. always_inline makes that frame
// the public entry point below rather than this helper, so the skipped frame
// is the same whether this is reached via unwind() or backtrace().
// _Unwind_Backtrace may leave a trailing null "top-most caller" that we trim.
__attribute__((always_inline))
static inline int do_unwind(void **pc, unsigned long *cfas, int nr)
{
    if (unwinding) {
        // Re-entered from an interrupt that landed inside libunwind, which
        // keeps state across the callback. Nothing here is worth the risk of
        // walking over it - the outer backtrace is the interesting one anyway.
        return 0;
    }

    // Written by the callback through a pointer, so it survives the longjmp
    // whether or not the compiler kept any of it in a register.
    worker_arg arg { pc, cfas, nr, -1, 0 };

    unwinding = true;
    if (!setjmp(unwind_recovery)) {
        _Unwind_Backtrace(worker, &arg);
    }
    unwinding = false;

    if (arg.pos > 0 && pc[arg.pos-1] == nullptr) {
        arg.pos--;
    }
    return arg.pos > 0 ? arg.pos : 0;
}

int osv::unwind(void **pc, unsigned long *cfa, int nr)
{
    return do_unwind(pc, cfa, nr);
}

int backtrace(void **buffer, int size)
{
    return do_unwind(buffer, nullptr, size);
}

#include <osv/demangle.hh>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

void backtrace_symbols_fd(void *const *addrs, int len, int fd)
{
    for (int i = 0; i < len; i++) {
        char name[1024];
        // FIXME: I think Linux also shows the name of the object
        osv::lookup_name_demangled(addrs[i], name, sizeof(name));
        auto remain = strlen(name);
        snprintf(name + remain, sizeof(name) - remain, " [%p]\n", addrs[i]);
        remain = strlen(name);
        while (remain > 0) {
            auto n = write(fd, name, remain);
            if (n < 0) {
                return; // write error, nothing better we can do...
            }
            remain -= n;
        }
    }
}
char **backtrace_symbols(void *const *addrs, int len)
{
    // We need to return a newly allocated char **, i.e., an array of len
    // pointers. We put the strings we point to after this array, allocated
    // together, so when the user free()s the array, the strings will also
    // be freed.
    size_t used = len * sizeof(char*);
    size_t bufsize = used + 1;
    char *buf = (char*)malloc(bufsize);
    char **ret = (char **)buf;
    for (int i = 0; i < len; i++) {
        char name[1024];
        // FIXME: I think Linux also shows the name of the object
        osv::lookup_name_demangled(addrs[i], name, sizeof(name));
        auto remain = strlen(name);
        snprintf(name + remain, sizeof(name) - remain, " [%p]", addrs[i]);
        remain = strlen(name);
        if (remain >= (bufsize - used)) {
            bufsize = bufsize*2 + remain + 1;
            buf = (char*)realloc(buf, bufsize);
            ret = (char **)buf;
        }
        ret[i] = buf + used;
        strcpy(buf + used, name);
        used += remain + 1;
    }
    return ret;
}
