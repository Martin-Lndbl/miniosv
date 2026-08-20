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

#include <malloc.h>
#include <sys/mman.h>

#include <osv/contiguous_alloc.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
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
        mem::frames::phys_addr pa = mem::frames::alloc();
        CHECK(pa != mem::frames::no_memory);
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
            mem::frames::phys_addr base = mmu::virt_to_phys(p);
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
               mem::frames::total_available_bytes() >> 20, after >> 20, n,
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
                std::vector<mem::frames::phys_addr> q(batch);
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

/* mapping ----------------------------------------------------------------- */

namespace map = mem::mapping;

// A reservation to write translations into. Nothing else hands these addresses
// out, and the tests below never leave one attached, so no fault ever lands in
// a region this file owns.
struct scratch {
    mem::vspace::region r;
    explicit scratch(size_t bytes, size_t align = page)
    {
        CHECK(mem::vspace::reserve(r, bytes, align) == resa::success);
    }
    ~scratch() { mem::vspace::release(r); }
    uintptr_t start() const { return r.span.start; }
    char *ptr(size_t off = 0) const { return reinterpret_cast<char*>(r.span.start + off); }
    mem::range range(size_t off, size_t len) const
    {
        return {r.span.start + off, r.span.start + off + len};
    }
};

void mapping_functional()
{
    group("mapping");

    section("an attached frame is reachable, and is gone once detached");
    {
        scratch s(page);
        auto f = mem::frames::alloc();
        CHECK(f != mem::frames::no_memory);

        map::attach(s.range(0, page), f, mem::perm_rw);
        auto e = map::find(s.start());
        CHECK(bool(e));
        CHECK(e.level() == 0);
        CHECK(e.addr() == f);
        CHECK(e.perm() & mem::perm_write);

        // The frame is reachable through both its linear address and the one
        // just attached, which is what an attachment means.
        s.ptr()[0] = 0x5a;
        CHECK(static_cast<char*>(mem::frames::to_linear(f))[0] == 0x5a);

        map::detach(s.range(0, page));
        CHECK(!map::find(s.start()));
        mem::frames::free(f);
    }

    section("attach refuses a range already mapped, attach_missing fills the gaps");
    {
        scratch s(4 * page);
        auto f = mem::frames::alloc();
        auto g = mem::frames::alloc();

        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        // The whole point of the return value: the second caller is told, and
        // the entry the first one wrote is still the one that is there.
        CHECK(!map::attach(s.range(0, page), g, mem::perm_rw));
        CHECK(map::find(s.start()).addr() == f);
        // A range that only overlaps in part is refused just the same, and
        // nothing in the untouched part of it is written.
        CHECK(!map::attach(s.range(0, 4 * page), g, mem::perm_rw));
        CHECK(!map::find(s.start() + page));

        // attach_missing accepts the overlap as long as it agrees, and fills
        // in what was not there. Only a different translation is an error.
        CHECK(!map::attach_missing(s.range(0, 4 * page), g, mem::perm_rw));
        CHECK(map::attach_missing(s.range(0, 4 * page), f, mem::perm_rw));
        CHECK(map::find(s.start()).addr() == f);
        for (unsigned i = 1; i < 4; i++) {
            auto e = map::find(s.start() + i * page);
            CHECK(bool(e));
            CHECK(e.addr() == f + i * page);
        }

        map::detach(s.range(0, 4 * page));
        mem::frames::free(f);
        mem::frames::free(g);
    }

    section("populate fills a range and depopulate gives the frames back");
    {
        scratch s(64 * page);
        size_t before = mem::frames::free_bytes();
        CHECK(map::populate(s.range(0, 64 * page), mem::perm_rw));

        for (unsigned i = 0; i < 64; i++) {
            auto e = map::find(s.start() + i * page);
            CHECK(bool(e));
            CHECK(s.ptr(i * page)[0] == 0);   // populate zeroes by default
            s.ptr(i * page)[0] = char(i);
        }
        for (unsigned i = 0; i < 64; i++) {
            CHECK(s.ptr(i * page)[0] == char(i));
        }
        CHECK(before - mem::frames::free_bytes() >= 64 * page);

        map::depopulate(s.range(0, 64 * page));
        CHECK(!map::find(s.start()));
        CHECK(mem::frames::free_bytes() >= before - page);
    }

    section("a huge-page range is one entry, and splits into 512");
    {
        scratch s(huge, huge);
        // An earlier owner of these addresses may have left a table behind: one
        // is only given back when a detach covers the whole of what it spans,
        // and a small mapping never does. Detaching the whole span is what
        // clears it, and without that the populate below could only build
        // 512 small leaves under the table that is already there.
        map::detach(s.range(0, huge));

        size_t before = mem::frames::free_bytes();
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));

        auto e = map::find(s.start());
        CHECK(bool(e));
        CHECK(e.level() == 1);
        CHECK(e.size() == huge);
        auto phys = e.addr();
        CHECK((phys & (huge - 1)) == 0);
        s.ptr(huge - 1)[0] = 0x7e;

        map::split(s.range(0, huge));
        // Every one of the small entries that replaced it maps its own slice of
        // the same frame, and the byte written through the large one is still
        // there.
        for (unsigned i = 0; i < 512; i += 64) {
            auto small = map::find(s.start() + i * page);
            CHECK(bool(small));
            CHECK(small.level() == 0);
            CHECK(small.addr() == phys + i * page);
        }
        CHECK(s.ptr(huge - 1)[0] == 0x7e);

        // A huge frame given back one small piece at a time has to come back
        // whole: the allocator handed out one block, and it is being returned
        // as 512 frames.
        map::depopulate(s.range(0, huge));
        CHECK(!map::find(s.start()));
        CHECK(mem::frames::free_bytes() >= before - page);
    }

    section("protect changes what an entry allows without moving the frame");
    {
        scratch s(page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        auto phys = map::find(s.start()).addr();
        s.ptr()[0] = 0x11;

        map::protect(s.range(0, page), mem::perm_read);
        CHECK(map::find(s.start()).perm() == mem::perm_read);
        CHECK(map::find(s.start()).addr() == phys);
        CHECK(s.ptr()[0] == 0x11);

        // Permissions of none keep the frame, which is what lets them be given
        // back later.
        map::protect(s.range(0, page), mem::perm_none);
        CHECK(map::find(s.start()).perm() == mem::perm_none);
        CHECK(map::find(s.start()).addr() == phys);

        map::protect(s.range(0, page), mem::perm_rw);
        CHECK(map::find(s.start()).perm() & mem::perm_write);
        CHECK(s.ptr()[0] == 0x11);

        map::depopulate(s.range(0, page));
    }

    section("prepare builds the levels, and the leaf is then one store away");
    {
        scratch s(page);
        auto e = map::prepare(s.start());
        CHECK(bool(e));
        CHECK(e.level() == 0);
        CHECK(e.empty());       // prepare writes no leaf of its own

        auto f = mem::frames::alloc();
        e.write(e.leaf_for(f, mem::perm_rw));
        map::barrier();
        s.ptr()[0] = 0x33;
        CHECK(static_cast<char*>(mem::frames::to_linear(f))[0] == 0x33);

        // The same slot, found the long way round.
        auto again = map::prepare(s.start());
        CHECK(again.addr() == f);
        CHECK(map::find(s.start()).addr() == f);

        map::detach(s.range(0, page));
        mem::frames::free(f);
    }

    section("the software bits of an entry survive a round trip");
    {
        scratch s(page);
        auto f = mem::frames::alloc();
        map::attach(s.range(0, page), f, mem::perm_rw);
        auto e = map::find(s.start());

        CHECK(map::sw_bits >= 3);   // the fewest any supported arch has
        for (unsigned n = 0; n < map::sw_bits; n++) {
            CHECK(!map::pte_sw_bit(e.read(), n));
            e.write(map::pte_set_sw_bit(e.read(), n, true));
            CHECK(map::pte_sw_bit(e.read(), n));
            CHECK(e.addr() == f);
            CHECK(e.perm() & mem::perm_write);
            e.write(map::pte_set_sw_bit(e.read(), n, false));
            CHECK(!map::pte_sw_bit(e.read(), n));
        }
        s.ptr()[0] = 0x44;      // still a usable mapping afterwards

        map::detach(s.range(0, page));
        mem::frames::free(f);
    }

    section("nothing is mapped where nothing was attached");
    {
        scratch s(16 * page);
        CHECK(!map::find(s.start()));
        CHECK(!map::find(s.start() + 15 * page));

        map::populate(s.range(page, page), mem::perm_rw);
        CHECK(!map::find(s.start()));
        CHECK(bool(map::find(s.start() + page)));
        CHECK(!map::find(s.start() + 2 * page));
        map::depopulate(s.range(page, page));
    }

    section("a global flush advances the epoch and a deferred detach does not");
    {
        scratch s(page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));

        auto before = map::flush_epoch();
        map::flush_all();
        CHECK(map::flush_epoch() > before);

        auto phys = map::find(s.start()).addr();
        before = map::flush_epoch();

        // The addresses come back in the caller's own record, so it can settle
        // exactly them, and nothing was invalidated on the way out.
        map::pending_invalidation stale;
        map::detach_deferred(s.range(0, page), stale);
        CHECK(stale.count == 1);
        CHECK(stale.va[0] == s.start());
        CHECK(!stale.all);
        CHECK(!map::find(s.start()));
        CHECK(map::flush_epoch() == before);

        // Two completed global invalidations settle the debt, not one: the
        // first may have been under way while the entry was still mapped.
        // Once they have happened, invalidate() has nothing left to do, and it
        // says so by not moving the epoch itself.
        map::flush_all();
        map::flush_all();
        auto quiet = map::flush_epoch();
        stale.invalidate();
        CHECK(map::flush_epoch() == quiet);
        CHECK(stale.count == 0);
        CHECK(!stale.all);
        mem::frames::free(phys);
    }
}

void mapping_perf()
{
    group("mapping - performance");

    section("attach and detach one page");
    {
        // attach() refuses a range that is already mapped, so each one has to
        // land on a page of its own; the detach that empties them again is
        // outside the measurement.
        const size_t pages = 512;
        const int rounds = 40;
        scratch s(pages * page);
        auto f = mem::frames::alloc();
        map::prepare(s.range(0, pages * page), page);

        double total = 0;
        for (int r = 0; r < rounds; r++) {
            auto t0 = clk::now();
            for (size_t i = 0; i < pages; i++) {
                map::attach(s.range(i * page, page), f, mem::perm_rw);
            }
            total += since(t0);
            map::detach(s.range(0, pages * page));
        }
        report_ns("attach 4 KiB, levels already built", total,
                  double(rounds) * pages);

        const int n = 20000;
        auto t0 = clk::now();
        for (int i = 0; i < n; i++) {
            map::attach(s.range(0, page), f, mem::perm_rw);
            map::detach(s.range(0, page));
        }
        report_ns("attach + detach 4 KiB, one global flush each", since(t0), n);

        mem::frames::free(f);
    }

    section("the prepared path: one store per page");
    {
        const size_t pages = 512;
        scratch s(pages * page);
        auto f = mem::frames::alloc();
        std::vector<map::pte_ref> slot(pages);

        auto t0 = clk::now();
        for (size_t i = 0; i < pages; i++) {
            slot[i] = map::prepare(s.start() + i * page);
        }
        report_ns("prepare, per 4 KiB page", since(t0), pages);

        const int rounds = 2000;
        t0 = clk::now();
        for (int r = 0; r < rounds; r++) {
            for (size_t i = 0; i < pages; i++) {
                slot[i].write(slot[i].leaf_for(f, mem::perm_rw));
            }
        }
        map::barrier();
        report_ns("write a prepared leaf", since(t0), double(rounds) * pages);

        map::detach(s.range(0, pages * page));
        mem::frames::free(f);
    }

    section("populate and depopulate");
    {
        struct { const char *name; size_t size; size_t leaf; int n; } cases[] = {
            {"64 pages, 4 KiB leaves", 64 * page, page, 2000},
            {"2 MiB, 4 KiB leaves",    huge,      page, 300},
            {"2 MiB, one huge leaf",   huge,      huge, 300},
        };
        for (auto &c : cases) {
            scratch s(c.size, c.leaf);
            auto t0 = clk::now();
            for (int i = 0; i < c.n; i++) {
                map::populate(s.range(0, c.size), mem::perm_rw, c.leaf);
                map::depopulate(s.range(0, c.size));
            }
            double s_total = since(t0);
            char label[80];
            snprintf(label, sizeof(label), "populate + depopulate %s", c.name);
            report_ns(label, s_total, c.n);
            snprintf(label, sizeof(label), "  the same, per 4 KiB page");
            report_ns(label, s_total, double(c.n) * (c.size / page));
        }
    }

    section("find");
    {
        scratch s(huge, huge);
        map::populate(s.range(0, huge), mem::perm_rw);
        const int probes = 200000;

        map::pte e = 0;
        auto t0 = clk::now();
        for (int i = 0; i < probes; i++) {
            e |= map::find(s.start() + (i % 512) * page).read();
        }
        escape(&e);
        report_ns("find, 4 KiB leaf", since(t0), probes);
        map::depopulate(s.range(0, huge));
    }
}

/* heap -------------------------------------------------------------------- */

/*
 * Only what needs to see inside. How the heap behaves as an allocator is in
 * os-memory.cc, through malloc, so that the same checks run on Linux and OSv;
 * what is here is the part of its design that is invisible from there -- that
 * a big allocation is one huge leaf rather than 512 small ones.
 */
void heap_functional()
{
    group("heap");

    section("a large allocation is backed by huge pages");
    {
        const size_t bytes = 5 * huge / 2;    // not a whole number of huge pages
        auto *p = static_cast<char *>(malloc(bytes));
        CHECK(p != nullptr);
        CHECK(malloc_usable_size(p) >= bytes);

        // One entry per 2 MiB, over a frame aligned to it. This is the whole
        // reason the heap asks for huge pages: 512 times fewer entries, and a
        // TLB that can cover the allocation.
        auto e = map::find(reinterpret_cast<uintptr_t>(p));
        CHECK(bool(e));
        CHECK(e.level() == 1);
        CHECK((e.addr() & (huge - 1)) == 0);

        // The last byte promised is as reachable as the first, which is what
        // rounding the mapping up to whole huge pages is for.
        memset(p, 0x3c, bytes);
        CHECK(p[bytes - 1] == 0x3c);
        free(p);
    }

    section("the heap gives its pages back when there is nothing else left");
    {
        // The heap keeps a page that empties, however many there are, because
        // taking it again is not free and nothing has said the memory is
        // wanted elsewhere. What makes that safe is frames::alloc() asking for
        // it before it fails, and this is the only thing that asks.
        const size_t size = 64ul << 10;
        size_t before = mem::frames::free_bytes();
        size_t n = before / 2 / size;
        std::vector<void *> p(n);
        for (size_t i = 0; i < n; i++) {
            p[i] = malloc(size);
        }
        for (size_t i = 0; i < n; i++) {
            free(p[i]);
        }
        size_t held = before - mem::frames::free_bytes();
        CHECK(held > before / 8);      // it really is holding it

        // Ask for more than is free but not for all of what it holds, so that
        // the rest of the kernel is never actually out of memory.
        size_t want = mem::frames::free_bytes() + held / 2;
        std::vector<mem::frames::phys_addr> blocks;
        blocks.reserve(want / huge + 1);
        bool ok = true;
        for (size_t got = 0; got < want && ok; got += huge) {
            auto f = mem::frames::alloc(huge, huge);
            ok = f != mem::frames::no_memory;
            if (ok) {
                blocks.push_back(f);
            }
        }
        CHECK(ok);
        printf("      heap held %zu MiB, then gave up %zu MiB of it\n",
               held >> 20, (blocks.size() * huge - (want - held / 2)) >> 20);
        for (auto f : blocks) {
            mem::frames::free(f, huge);
        }
    }

    section("the pages under an allocation are given back");
    {
        const size_t bytes = 8 * huge;
        size_t before = mem::frames::free_bytes();
        void *p = malloc(bytes);
        CHECK(p != nullptr);
        memset(p, 0x5a, bytes);
        CHECK(before - mem::frames::free_bytes() >= bytes);
        free(p);
        // The mapping goes with the frames, so nothing can be reached there.
        CHECK(!map::find(reinterpret_cast<uintptr_t>(p)));
        CHECK(mem::frames::free_bytes() >= before - huge);
    }
}

}

int os_memory_primitives_main()
{
    reset();
    printf("######## memory primitives ########\n");
    printf("cpus: %u, memory: %zu MiB\n", n_cpus(), mem::frames::total_available_bytes() >> 20);

    frames_functional();
    frames_perf();
    vspace_functional();
    vspace_perf();
    mapping_functional();
    mapping_perf();
    heap_functional();

    return summary("MEMORY PRIMITIVE");
}
