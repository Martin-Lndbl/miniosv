/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * x86-64 specific memory operations and formats.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>
#include <vector>

#include <osv/clock.hh>
#include <osv/interrupt.hh>
#include <osv/mem/mapping.hh>
#include <osv/migration-lock.hh>
#include <osv/sched.hh>

#include "arch-cpu.hh"

namespace mem {
namespace mapping {

// Root of the page table.
static std::atomic<pte> page_table_root;

std::atomic<pte> *root_slot(uintptr_t)
{
    return &page_table_root;
}

void tlb_flush_page(uintptr_t va)
{
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void tlb_flush_pages(const uintptr_t *va, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        tlb_flush_page(va[i]);
    }
}

void tlb_flush_local()
{
    // TODO: we can use the root table instead of read_cr3(), can be faster
    // when shadow page tables are used.
    processor::write_cr3(processor::read_cr3());
}

static mutex flush_mutex;
static sched::thread_handle flush_waiter;
static std::atomic<int> flush_pendingconfirms;

// List of addresses to flush.
static std::atomic<const uintptr_t *> flush_va;
static std::atomic<size_t> flush_va_count;

static inter_processor_interrupt flush_ipi{IPI_TLB_FLUSH, [] {
        auto *va = flush_va.load(std::memory_order_acquire);
        if (va) {
            tlb_flush_pages(va, flush_va_count.load(std::memory_order_relaxed));
        } else {
            tlb_flush_local();
        }
        if (flush_pendingconfirms.fetch_add(-1) == 1) {
            flush_waiter.wake_from_kernel_or_with_irq_disabled();
        }
}};

// See shootdown_stats in arch/common/mem.hh. Relaxed throughout: these are
// read once, after the run that produced them, and an exact order between
// two cpus' increments would say nothing a sum does not.
static std::atomic<uint64_t> sd_count, sd_pages, sd_all, sd_ns_total, sd_ns_max;

shootdown_stats tlb_shootdown_stats()
{
    return shootdown_stats{
        sd_count.load(std::memory_order_relaxed),
        sd_pages.load(std::memory_order_relaxed),
        sd_all.load(std::memory_order_relaxed),
        sd_ns_total.load(std::memory_order_relaxed),
        sd_ns_max.load(std::memory_order_relaxed),
    };
}

/*
 * Shoot down TLB entries on all CPUs. If va is nullptr, flush the entire TLB.
 */
static void shootdown(const uintptr_t *va, size_t count)
{
    auto flush_here = [va, count] {
        if (va) {
            tlb_flush_pages(va, count);
        } else {
            tlb_flush_local();
        }
    };

    if (sched::cpus.size() <= 1) {
        flush_here();
        return;
    }

    // Timed from here, not from the top: the single-cpu case above is a local
    // invalidate and none of what this measures applies to it.
    sd_count.fetch_add(1, std::memory_order_relaxed);
    sd_pages.fetch_add(count, std::memory_order_relaxed);
    if (!va) {
        sd_all.fetch_add(1, std::memory_order_relaxed);
    }
    auto t0 = osv::clock::uptime::now();
    // Not a scope guard: the wait below is the thing being measured, so the
    // clock has to be read after it, and there is no early return past it.

    SCOPE_LOCK(migration_lock);
    std::lock_guard<mutex> guard(flush_mutex);
    flush_waiter.reset(*sched::thread::current());
    // Every cpu, whatever it runs: kernel memory is unmapped and reused too
    // (the heap gives its pages back), so a cpu running a kernel thread may
    // hold a stale entry and touch it before it next switches threads.
    int confirms = sched::cpus.size() - 1;

    flush_va_count.store(count, std::memory_order_relaxed);
    flush_va.store(va, std::memory_order_release);
    flush_pendingconfirms.store(confirms);
    flush_ipi.send_allbutself();

    flush_here();

    sched::thread::wait_until([] {
            return flush_pendingconfirms.load() == 0;
    });
    flush_waiter.clear();

    uint64_t ns = (osv::clock::uptime::now() - t0).count();
    sd_ns_total.fetch_add(ns, std::memory_order_relaxed);
    // fetch_max, spelled out: the store must not undo a larger value another
    // cpu put there between the load and the exchange.
    uint64_t seen = sd_ns_max.load(std::memory_order_relaxed);
    while (ns > seen &&
           !sd_ns_max.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
}

void tlb_flush_all()
{
    shootdown(nullptr, 0);
}

void tlb_flush_pages_all(const uintptr_t *va, size_t count)
{
    shootdown(va, count);
}

}
}

namespace mem {
namespace mapping {

void switch_to_runtime_page_tables()
{
    auto root = root_slot(0)->load(std::memory_order_acquire);
    processor::write_cr3(pte_table_addr(root));
}

}
}

namespace mem {
namespace mapping {

uint8_t phys_bits = max_phys_bits, virt_bits = 52;

}
}
