/*
 * Where a fault goes.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <atomic>

#include <osv/clock.hh>
#include <osv/debug.hh>
#include <osv/mem/fault.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>
#include <osv/trace.hh>

#include "exceptions.hh"
#include "dump.hh"
#include "libc/signal.hh"

extern const char text_start[], text_end[];

namespace mem {

TRACEPOINT(trace_vm_fault, "addr=%p, error_code=%x", uintptr_t, unsigned int);
TRACEPOINT(trace_vm_fault_sigsegv, "addr=%p, error_code=%x, %s", uintptr_t, unsigned int, const char*);
TRACEPOINT(trace_vm_fault_ret, "addr=%p, error_code=%x", uintptr_t, unsigned int);

// See fault_stats in <osv/mem/fault.hh>.
static std::atomic<uint64_t> fault_count, fault_sigsegv, fault_ns_total, fault_ns_max;

namespace {

void sigsegv(uintptr_t addr, exception_frame *ef)
{
    void *pc = ef->get_pc();
    if (pc >= text_start && pc < text_end) {
        debug_ll("page fault outside application, addr: 0x%016lx\n", addr);
        // Which of the two shapes this is decides where to look next, and the
        // address alone cannot say. A reservation still covering the address
        // means the mapping went away under memory that is still owned --
        // populate/depopulate territory. No reservation means the address is
        // stale: whoever held it kept it past the release.
        auto *r = vspace::lookup(addr);
        if (r) {
            debug_ll("  in region [0x%016lx, 0x%016lx) %lu KiB perm=%x fault_ops=%d\n",
                     r->span.start, r->span.end, r->span.size() >> 10, r->perm,
                     r->ops && r->ops->fault ? 1 : 0);
            auto e = mapping::find(addr);
            if (e) {
                // The region says what the memory is allowed to be used for;
                // the PTE says what the hardware will actually allow. When a
                // write faults on a present leaf, those two disagree, and
                // which way they disagree is the whole question.
                debug_ll("  leaf: present level=%u perm=%x present_bit=%d phys=0x%016lx\n",
                         e.level(), e.perm(), e.present() ? 1 : 0,
                         (unsigned long)e.addr());
            } else {
                debug_ll("  leaf: ABSENT (unmapped under a live region)\n");
            }
            debug_ll("  fault: error=0x%x (%s, %s)\n", ef->get_error(),
                     mapping::is_page_fault_write(ef->get_error()) ? "write" : "read",
                     mapping::is_page_fault_insn(ef->get_error()) ? "insn" : "data");
        } else {
            debug_ll("  no reservation covers this address (stale pointer)\n");
        }
        dump_registers(ef);
        abort();
    }
    osv::handle_mmap_fault(addr, SIGSEGV, ef);
}

bool permitted(unsigned perm, unsigned error_code)
{
    if (mapping::is_page_fault_insn(error_code)) {
        return perm & perm_exec;
    }
    if (mapping::is_page_fault_write(error_code)) {
        return perm & perm_write;
    }
    return perm & perm_read;
}

}

/*
 * A fault is a region lookup and whatever that region wants done about it.
 *
 * The address is passed on to the byte. Rounding it to a page here would be
 * deciding something on the region's behalf, and a region whose units are not
 * pages needs the byte to tell which of them was reached for.
 */
void vm_fault(uintptr_t addr, exception_frame *ef)
{
    unsigned error = ef->get_error();
    trace_vm_fault(addr, error);
    fault_count.fetch_add(1, std::memory_order_relaxed);
    auto t0 = osv::clock::uptime::now();
    // Charged even when this ends in SIGSEGV: the two paths leave through
    // different returns, and a fault that cost time still cost it.
    auto charge = [t0] {
        uint64_t ns = (osv::clock::uptime::now() - t0).count();
        fault_ns_total.fetch_add(ns, std::memory_order_relaxed);
        uint64_t seen = fault_ns_max.load(std::memory_order_relaxed);
        while (ns > seen && !fault_ns_max.compare_exchange_weak(
                                seen, ns, std::memory_order_relaxed)) {
        }
    };

    if (mapping::fast_sigsegv_check(addr, ef)) {
        fault_sigsegv.fetch_add(1, std::memory_order_relaxed);
        charge();
        sigsegv(addr, ef);
        trace_vm_fault_sigsegv(addr, error, "fast");
        return;
    }

    // A region without a fault handler has nothing to answer with.
    auto *r = vspace::lookup(addr);
    if (!r || !r->ops || !r->ops->fault || !permitted(r->perm, error) ||
        !r->ops->fault(*r, addr, error)) {
        fault_sigsegv.fetch_add(1, std::memory_order_relaxed);
        charge();
        sigsegv(addr, ef);
        trace_vm_fault_sigsegv(addr, error, "slow");
        return;
    }
    charge();
    trace_vm_fault_ret(addr, error);
}

fault_stats vm_fault_stats()
{
    return fault_stats{
        fault_count.load(std::memory_order_relaxed),
        fault_sigsegv.load(std::memory_order_relaxed),
        fault_ns_total.load(std::memory_order_relaxed),
        fault_ns_max.load(std::memory_order_relaxed),
    };
}

}
