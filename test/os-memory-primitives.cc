/*
 * The memory primitives, which only miniOSv has: the frame allocator and the
 * address space layer, called directly rather than through malloc and mmap.
 *
 * The test application is linked into the kernel, so it can call them at all;
 * that it can is itself part of what is being checked, since the point of the
 * layers is that an application can reach past the defaults and manage memory
 * itself. What an ordinary program sees is in os-memory.cc.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <osv/contiguous_alloc.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/vspace.hh>
#include <osv/mempool.hh>
#include <osv/mmu.hh>

#include "mem-test.hh"

using namespace memtest;

namespace {

const size_t page = mem::frames::page_size;
const size_t huge = mmu::huge_page_size;

using resa = mem::vspace::resa_result;

/* frames ------------------------------------------------------------------ */

void frames_functional()
{
    group("frames");

    section("frames are distinct, aligned and writable");
    {
        const int n = 64;
        void *p[n];
        for (int i = 0; i < n; i++) {
            p[i] = memory::alloc_page();
            CHECK(p[i] != nullptr);
            CHECK(mmu::is_page_aligned(p[i]));
            memset(p[i], 0xa5, page);
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                CHECK(p[i] != p[j]);
            }
        }
        for (int i = 0; i < n; i++) {
            CHECK(static_cast<unsigned char *>(p[i])[page - 1] == 0xa5);
            memory::free_page(p[i]);
        }
    }

    section("a frame is in the linear map and round-trips to its address");
    {
        mem::phys_addr pa = mem::frames::alloc();
        CHECK(pa != mem::no_memory);
        void *v = mem::frames::to_linear(pa);
        CHECK(mem::frames::from_linear(v) == pa);
        CHECK(mmu::is_linear_mapped(v, page));
        CHECK((pa & (page - 1)) == 0);
        mem::frames::free(pa);
    }

    section("a 2 MiB frame is 2 MiB aligned");
    {
        void *h = memory::alloc_huge_page(huge);
        CHECK(h != nullptr);
        if (h) {
            CHECK((reinterpret_cast<uintptr_t>(h) & (huge - 1)) == 0);
            memset(h, 0x5a, huge);
            memory::free_huge_page(h, huge);
        }
    }

    section("a contiguous allocation really is contiguous");
    {
        const size_t sizes[] = {1ul << 20, 8ul << 20};
        for (size_t size : sizes) {
            void *p = memory::alloc_phys_contiguous_aligned(size, page);
            CHECK(p != nullptr);
            if (!p) {
                continue;
            }
            mmu::phys base = mmu::virt_to_phys(p);
            bool ok = true;
            for (size_t off = 0; off < size; off += page) {
                ok = ok && mmu::virt_to_phys(static_cast<char *>(p) + off) == base + off;
            }
            CHECK(ok);
            memory::free_phys_contiguous_aligned(p, size);
        }
    }

    section("free memory falls while frames are out and returns after");
    {
        // The per-cpu page pools sit between alloc_page and the accounting, so
        // the counter lags by up to a pool's worth. The drift is printed rather
        // than asserted tightly.
        const size_t slack = 64ul << 20;
        size_t before = mem::frames::free_bytes();
        const int n = 4096;
        std::vector<void *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = memory::alloc_page();
        }
        size_t during = mem::frames::free_bytes();
        CHECK(during <= before);
        for (int i = 0; i < n; i++) {
            memory::free_page(p[i]);
        }
        size_t after = mem::frames::free_bytes();
        CHECK(after >= during);
        CHECK(after + slack >= before);
        printf("      total %zu MiB, free %zu MiB, drift after %d pages: %ld KiB\n",
               mem::frames::total_bytes() >> 20, after >> 20, n,
               (static_cast<long>(before) - static_cast<long>(after)) >> 10);
    }
}

void frames_perf()
{
    group("frames - performance");

    const int batch = 512;
    const int rounds = 100;
    std::vector<void *> p(batch);

    section("4 KiB, scaling");
    {
        // Warm the per-cpu pools so the first measurement is not the only one
        // paying for a refill.
        for (int i = 0; i < batch; i++) {
            p[i] = memory::alloc_page();
        }
        for (int i = 0; i < batch; i++) {
            memory::free_page(p[i]);
        }

        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned) {
                std::vector<void *> q(batch);
                for (int r = 0; r < rounds; r++) {
                    for (int i = 0; i < batch; i++) {
                        q[i] = memory::alloc_page();
                        escape(q[i]);
                    }
                    for (int i = 0; i < batch; i++) {
                        memory::free_page(q[i]);
                    }
                }
            });
            report_scale("alloc_page + free_page", t, 2.0 * batch * rounds * t, s);
        }
    }

    section("4 KiB through mem::frames, no pool in the way");
    {
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned) {
                std::vector<mem::phys_addr> q(batch);
                for (int r = 0; r < rounds; r++) {
                    for (int i = 0; i < batch; i++) {
                        q[i] = mem::frames::alloc();
                    }
                    for (int i = 0; i < batch; i++) {
                        mem::frames::free(q[i]);
                    }
                }
            });
            report_scale("frames::alloc + free", t, 2.0 * batch * rounds * t, s);
        }
    }

    section("cpu spread");
    {
        // A scaling run that lands on one cpu looks exactly like a global lock,
        // so check the threads really are spread out.
        unsigned t = std::min(n_cpus(), 8u);
        std::vector<unsigned> seen(t, ~0u);
        parallel(t, [&](unsigned id) { seen[id] = sched::cpu::current()->id; });
        unsigned distinct = 0;
        for (unsigned i = 0; i < t; i++) {
            bool first = true;
            for (unsigned j = 0; j < i; j++) {
                first = first && seen[j] != seen[i];
            }
            distinct += first;
        }
        CHECK(distinct == t);
        printf("      %u threads on %u distinct cpus\n", t, distinct);
    }

    section("2 MiB, scaling");
    {
        const int hbatch = 16;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(hbatch * rounds, t, 8);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    void *h = memory::alloc_huge_page(huge);
                    escape(h);
                    if (h) {
                        memory::free_huge_page(h, huge);
                    }
                }
            });
            report_scale("alloc_huge_page + free", t, 2.0 * n * t, s);
        }
    }

    section("contiguous");
    {
        const int n = 64;
        for (size_t size : {1ul << 20, 8ul << 20, 64ul << 20}) {
            auto t0 = clk::now();
            int got = 0;
            for (int i = 0; i < n; i++) {
                void *q = memory::alloc_phys_contiguous_aligned(size, page);
                if (!q) {
                    break;
                }
                escape(q);
                memory::free_phys_contiguous_aligned(q, size);
                got++;
            }
            char label[64];
            snprintf(label, sizeof(label), "alloc_phys_contiguous %zu MiB + free", size >> 20);
            if (got) {
                report_ns(label, since(t0), got);
            } else {
                printf("    %-46s %9s\n", label, "failed");
            }
        }
    }
}

/* vspace ------------------------------------------------------------------ */

void vspace_functional()
{
    group("vspace");

    section("reserving costs no physical memory");
    {
        mem::vspace::region r{};
        size_t before = mem::frames::free_bytes();
        CHECK(mem::vspace::reserve(r, 64ul << 20, page) == resa::success);
        CHECK(before - mem::frames::free_bytes() < (1ul << 20));
        CHECK(mem::vspace::reserved(r.span));
        mem::range span = r.span;
        mem::vspace::release(r);
        CHECK(!mem::vspace::reserved(span));
    }

    section("a reservation is aligned as asked and lands in the window");
    {
        for (size_t align : {page, huge, 1ul << 30}) {
            mem::vspace::region r{};
            CHECK(mem::vspace::reserve(r, huge, align) == resa::success);
            CHECK((r.span.start & (align - 1)) == 0);
            CHECK(r.span.size() == huge);
            CHECK(mem::vspace::app_window().contains(r.span));
            mem::vspace::release(r);
        }
    }

    section("reserve_at is exact, and refuses a range already taken");
    {
        const size_t size = 2ul << 20;
        mem::vspace::region a{}, b{};
        CHECK(mem::vspace::reserve(a, size, size) == resa::success);
        CHECK(mem::vspace::reserve_at(b, a.span) == resa::already_reserved);
        CHECK(mem::vspace::reserve_at(b, {a.span.start + size / 2, a.span.end + size}) ==
              resa::already_reserved);
        mem::range span = a.span;
        mem::vspace::release(a);
        CHECK(mem::vspace::reserve_at(b, span) == resa::success);
        CHECK(b.span.start == span.start);
        mem::vspace::release(b);
    }

    section("lookup finds the region holding an address, edges included");
    {
        mem::vspace::region r{};
        const size_t size = 2ul << 20;
        CHECK(mem::vspace::reserve(r, size, page) == resa::success);
        uintptr_t a = r.span.start;
        CHECK(mem::vspace::lookup(a) == &r);
        CHECK(mem::vspace::lookup(a + size - 1) == &r);
        CHECK(mem::vspace::lookup(a + size) != &r);
        CHECK(mem::vspace::lookup(a - 1) != &r);
        mem::vspace::release(r);
        CHECK(mem::vspace::lookup(a) == nullptr);
    }

    section("lookup answers for every address in a region, and none outside");
    {
        mem::vspace::region r{};
        const size_t size = 2ul << 20;
        CHECK(mem::vspace::reserve(r, size, page) == resa::success);
        bool inside = true;
        for (size_t off = 0; off < size; off += page) {
            inside = inside && mem::vspace::lookup(r.span.start + off) == &r;
        }
        CHECK(inside);
        CHECK(mem::vspace::lookup(r.span.end) != &r);
        r.perm = mem::perm_read;
        CHECK(mem::vspace::lookup(r.span.start)->perm == mem::perm_read);
        mem::vspace::release(r);
    }

    section("the index holds up with 10000 live reservations");
    {
        const int n = 10000;
        // A registered region may not move: the index holds a pointer to it.
        std::vector<mem::vspace::region> r(n);
        int got = 0;
        for (int i = 0; i < n; i++) {
            if (mem::vspace::reserve(r[i], page, page) == resa::success) {
                got++;
            }
        }
        CHECK(got == n);
        CHECK(mem::vspace::self_check());
        for (int i = 0; i < n; i += 97) {
            CHECK(mem::vspace::lookup(r[i].span.start) == &r[i]);
        }
        for (int i = 0; i < n; i++) {
            mem::vspace::release(r[i]);
        }
        CHECK(mem::vspace::self_check());
    }

    section("the index survives a random reserve/release sequence");
    {
        const int n = 512;
        std::vector<mem::vspace::region> r(n);
        std::vector<bool> live(n, false);
        uint32_t seed = 20260819;
        auto rnd = [&seed] {
            seed = seed * 1103515245u + 12345u;
            return seed >> 8;
        };
        for (int i = 0; i < 4000; i++) {
            int at = rnd() % n;
            if (live[at]) {
                mem::vspace::release(r[at]);
                live[at] = false;
            } else {
                size_t size = (rnd() % 16 + 1) * page;
                live[at] = mem::vspace::reserve(r[at], size, page) == resa::success;
            }
        }
        CHECK(mem::vspace::self_check());
        for (int i = 0; i < n; i++) {
            if (live[i]) {
                mem::vspace::release(r[i]);
            }
        }
        CHECK(mem::vspace::self_check());
    }

    section("lookups stay correct while the index is rebuilt underneath them");
    {
        const int stable_n = 256;
        const int churn_n = 256;
        std::vector<mem::vspace::region> stable(stable_n);
        std::vector<mem::vspace::region> churn(churn_n);
        for (int i = 0; i < stable_n; i++) {
            CHECK(mem::vspace::reserve(stable[i], page, page) == resa::success);
        }

        std::atomic<bool> stop{false};
        std::atomic<long> wrong{0};
        std::atomic<long> missing{0};
        std::atomic<long> done{0};
        unsigned threads = std::max(2u, std::min(n_cpus(), 16u));

        parallel(threads, [&](unsigned id) {
            if (id == 0) {
                std::vector<bool> live(churn_n, false);
                uint32_t seed = 99194853;
                for (int round = 0; round < 20000; round++) {
                    seed = seed * 1103515245u + 12345u;
                    int at = (seed >> 8) % churn_n;
                    if (live[at]) {
                        mem::vspace::release(churn[at]);
                        live[at] = false;
                    } else {
                        live[at] = mem::vspace::reserve(churn[at], page, page) == resa::success;
                    }
                }
                for (int i = 0; i < churn_n; i++) {
                    if (live[i]) {
                        mem::vspace::release(churn[i]);
                    }
                }
                stop.store(true);
                return;
            }
            uint32_t seed = id * 2654435761u + 1;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 64; k++) {
                    seed = seed * 1103515245u + 12345u;
                    int i = (seed >> 8) % stable_n;
                    uintptr_t a = stable[i].span.start;
                    auto *found = mem::vspace::lookup(a);
                    if (!found) {
                        missing.fetch_add(1, std::memory_order_relaxed);
                    } else if (found != &stable[i]) {
                        wrong.fetch_add(1, std::memory_order_relaxed);
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        CHECK(done.load() > 0);
        CHECK(missing.load() == 0);
        CHECK(wrong.load() == 0);
        CHECK(mem::vspace::self_check());
        printf("      %ld lookups against 20000 reserve/release rounds\n", done.load());
        for (int i = 0; i < stable_n; i++) {
            mem::vspace::release(stable[i]);
        }
        CHECK(mem::vspace::self_check());
    }

    section("reservations from several threads do not overlap");
    {
        const int per_thread = 16;
        unsigned threads = n_cpus();
        std::vector<mem::vspace::region> r(threads * per_thread);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                mem::vspace::reserve(r[id * per_thread + i], 1ul << 20, page);
            }
        });
        std::vector<mem::range> spans;
        for (auto &e : r) {
            CHECK(!e.span.empty());
            spans.push_back(e.span);
        }
        std::sort(spans.begin(), spans.end(),
                  [](const mem::range &a, const mem::range &b) { return a.start < b.start; });
        for (size_t i = 1; i < spans.size(); i++) {
            CHECK(spans[i - 1].end <= spans[i].start);
        }
        CHECK(mem::vspace::self_check());
        for (auto &e : r) {
            mem::vspace::release(e);
        }
    }

    section("the layer counts what it holds");
    {
        struct counter {
            size_t regions;
            size_t bytes;
            uintptr_t last_end;
            bool ordered;
        };
        const size_t size = 1ul << 20;
        const int n = 8;
        std::vector<mem::vspace::region> r(n);
        size_t regions_before = mem::vspace::count();
        size_t bytes_before = mem::vspace::reserved_bytes();
        for (int i = 0; i < n; i++) {
            CHECK(mem::vspace::reserve(r[i], size, page) == resa::success);
        }
        CHECK(mem::vspace::count() == regions_before + n);
        CHECK(mem::vspace::reserved_bytes() == bytes_before + n * size);

        counter c{0, 0, 0, true};
        mem::vspace::for_each([](const mem::vspace::region &e, void *arg) {
            auto *s = static_cast<counter *>(arg);
            s->regions++;
            s->bytes += e.span.size();
            s->ordered = s->ordered && e.span.start >= s->last_end;
            s->last_end = e.span.end;
        }, &c);
        CHECK(c.ordered);
        CHECK(c.regions == mem::vspace::count());
        CHECK(c.bytes == mem::vspace::reserved_bytes());

        for (int i = 0; i < n; i++) {
            mem::vspace::release(r[i]);
        }
        CHECK(mem::vspace::count() == regions_before);
        CHECK(mem::vspace::reserved_bytes() == bytes_before);
    }
}

void vspace_perf()
{
    group("vspace - performance");

    section("reserve + release");
    {
        struct { const char *name; size_t size; int n; } cases[] = {
            {"4 KiB",  4ul << 10, 20000},
            {"2 MiB",  2ul << 20, 20000},
            {"64 MiB", 64ul << 20, 2000},
        };
        for (auto &c : cases) {
            mem::vspace::region r{};
            auto t0 = clk::now();
            for (int i = 0; i < c.n; i++) {
                mem::vspace::reserve(r, c.size, page);
                mem::vspace::release(r);
            }
            char label[64];
            snprintf(label, sizeof(label), "reserve + release %s", c.name);
            report_ns(label, since(t0), c.n);
        }
    }

    section("reserve + release, scaling");
    {
        const int total = 40000;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t, 64);
            double s = parallel(t, [&](unsigned) {
                mem::vspace::region r{};
                for (int i = 0; i < n; i++) {
                    mem::vspace::reserve(r, 2ul << 20, page);
                    mem::vspace::release(r);
                }
            });
            report_scale("reserve + release 2 MiB", t, static_cast<double>(n) * t, s);
        }
    }

    section("lookup");
    {
        for (int live : {1, 100, 1000, 10000}) {
            std::vector<mem::vspace::region> r(live);
            for (int i = 0; i < live; i++) {
                mem::vspace::reserve(r[i], page, page);
            }
            const int probes = 200000;
            uintptr_t target = r[live / 2].span.start;
            auto t0 = clk::now();
            for (int i = 0; i < probes; i++) {
                escape(mem::vspace::lookup(target));
            }
            char label[64];
            snprintf(label, sizeof(label), "lookup with %d live regions", live);
            report_ns(label, since(t0), probes);
            for (int i = 0; i < live; i++) {
                mem::vspace::release(r[i]);
            }
        }
    }

    section("lookup, scaling");
    {
        const int live = 1000;
        std::vector<mem::vspace::region> r(live);
        for (int i = 0; i < live; i++) {
            mem::vspace::reserve(r[i], page, page);
        }
        const int probes = 200000;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned id) {
                uintptr_t target = r[(id * 37) % live].span.start;
                for (int i = 0; i < probes; i++) {
                    escape(mem::vspace::lookup(target));
                }
            });
            report_scale("lookup, 1000 live regions", t,
                         static_cast<double>(probes) * t, s);
        }
        for (int i = 0; i < live; i++) {
            mem::vspace::release(r[i]);
        }
    }

}

}

int os_memory_primitives_main()
{
    reset();
    printf("######## memory primitives ########\n");
    printf("cpus: %u, memory: %zu MiB\n", n_cpus(), mem::frames::total_bytes() >> 20);

    frames_functional();
    frames_perf();
    vspace_functional();
    vspace_perf();

    return summary("MEMORY PRIMITIVE");
}
