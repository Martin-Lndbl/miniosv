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
#include <map>
#include <thread>
#include <vector>

#include <malloc.h>
#include <sys/mman.h>

#include <osv/align.hh>
#include <osv/mem/early.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/pagecache.hh>
#include <osv/mem/phys.hh>
#include <osv/mem/store.hh>
#include <osv/mem/vspace.hh>

#include "core/mem/linear.hh"
#include "mem-test.hh"

using namespace memtest;

namespace {

const size_t page = mem::frames::page_size;
const size_t huge = mem::mapping::huge_page_size;

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
            p[i] = mem::frames::to_linear(mem::frames::alloc());
            CHECK(p[i] != nullptr);
            CHECK((reinterpret_cast<uintptr_t>(p[i]) % page) == 0);
            memset(p[i], 0xa5, page);
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                CHECK(p[i] != p[j]);
            }
        }
        for (int i = 0; i < n; i++) {
            CHECK(static_cast<unsigned char *>(p[i])[page - 1] == 0xa5);
            mem::frames::free(mem::frames::from_linear(p[i]));
        }
    }

    section("a frame is in the linear map and round-trips to its address");
    {
        mem::frames::phys_addr pa = mem::frames::alloc();
        CHECK(pa != mem::frames::no_memory);
        void *v = mem::frames::to_linear(pa);
        CHECK(mem::frames::from_linear(v) == pa);
        CHECK(mem::frames::in_linear_map(v, page));
        CHECK((pa & (page - 1)) == 0);
        mem::frames::free(pa);
    }

    section("a 2 MiB frame is 2 MiB aligned");
    {
        void *h = mem::frames::to_linear(mem::frames::alloc(huge, huge));
        CHECK(h != nullptr);
        if (h) {
            CHECK((reinterpret_cast<uintptr_t>(h) & (huge - 1)) == 0);
            memset(h, 0x5a, huge);
            mem::frames::free(mem::frames::from_linear(h), huge);
        }
    }

    section("a contiguous allocation really is contiguous");
    {
        const size_t sizes[] = {1ul << 20, 8ul << 20};
        for (size_t size : sizes) {
            mem::frames::phys_addr pa = mem::frames::alloc(size, page);
            void *p = pa ? mem::map_phys(pa, size) : nullptr;
            CHECK(p != nullptr);
            if (!p) {
                continue;
            }
            mem::frames::phys_addr base = mem::mapping::to_phys(p);
            bool ok = true;
            for (size_t off = 0; off < size; off += page) {
                ok = ok && mem::mapping::to_phys(static_cast<char *>(p) + off) == base + off;
            }
            CHECK(ok);
            mem::frames::free(pa, size);
        }
    }

    section("a pressure watcher registered by the application is asked");
    {
        // The list keeps the watcher forever, so it must not live on the stack.
        static std::atomic<int> asked{0};
        static mem::frames::pressure_watcher w;
        mem::frames::watch_pressure(w, [] {
            asked.fetch_add(1);
            return false;
        }, 99);
        int before = asked.load();
        mem::frames::reclaim();
        CHECK(asked.load() > before);
    }

    section("free memory falls while frames are out and returns after");
    {
        // The per-cpu pools sit between frames::alloc and the accounting, so the
        // counter lags by up to a pool's worth. The drift is printed rather than
        // asserted tightly.
        const size_t slack = 64ul << 20;
        size_t before = mem::frames::free_bytes();
        const int n = 4096;
        std::vector<void *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = mem::frames::to_linear(mem::frames::alloc());
        }
        size_t during = mem::frames::free_bytes();
        CHECK(during <= before);
        for (int i = 0; i < n; i++) {
            mem::frames::free(mem::frames::from_linear(p[i]));
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

    section("4 KiB, scaling");
    {
        // Warm the per-cpu pools so the first measurement is not the only one
        // paying for a refill.
        std::vector<mem::frames::phys_addr> p(batch);
        for (int i = 0; i < batch; i++) {
            p[i] = mem::frames::alloc();
        }
        for (int i = 0; i < batch; i++) {
            mem::frames::free(p[i]);
        }

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
                    void *h = mem::frames::to_linear(mem::frames::alloc(huge, huge));
                    escape(h);
                    if (h) {
                        mem::frames::free(mem::frames::from_linear(h), huge);
                    }
                }
            });
            report_scale("frames::alloc(2 MiB) + free", t, 2.0 * n * t, s);
        }
    }

    section("contiguous");
    {
        const int n = 64;
        for (size_t size : {1ul << 20, 8ul << 20, 64ul << 20}) {
            auto t0 = clk::now();
            int got = 0;
            for (int i = 0; i < n; i++) {
                mem::frames::phys_addr qa = mem::frames::alloc(size, page);
                void *q = qa ? mem::map_phys(qa, size) : nullptr;
                if (!q) {
                    break;
                }
                escape(q);
                mem::frames::free(qa, size);
                got++;
            }
            char label[64];
            snprintf(label, sizeof(label), "frames::alloc %zu MiB + map + free", size >> 20);
            if (got) {
                report_ns(label, since(t0), got);
            } else {
                printf("    %-46s %9s\n", label, "failed");
            }
        }
    }
}

/* vspace ------------------------------------------------------------------ */

// A region whose faults the test answers itself: one page per fault, counted.
struct probe_region {
    mem::vspace::region r;
    std::atomic<int> faults{0};
};

bool probe_fault(mem::vspace::region &r, uintptr_t addr, unsigned)
{
    auto *pr = reinterpret_cast<probe_region *>(&r);
    pr->faults.fetch_add(1);
    uintptr_t s = align_down(addr, uintptr_t(page));
    return mem::mapping::populate({s, s + page}, r.perm) ||
           mem::mapping::find(s);
}

const mem::vspace::region_ops probe_ops = { probe_fault };

void vspace_functional()
{
    group("vspace");

    section("a fault in a region goes to the handler its owner plugged in");
    {
        probe_region pr;
        pr.r.ops = &probe_ops;
        pr.r.perm = mem::perm_rw;
        CHECK(mem::vspace::reserve(pr.r, 16 * page, page) == resa::success);

        auto *p = reinterpret_cast<volatile char *>(pr.r.span.start);
        p[0] = 1;
        p[3 * page] = 3;
        CHECK(pr.faults.load() == 2);
        CHECK(p[0] == 1);
        CHECK(p[3 * page] == 3);
        // The pages the handler did not map are still absent.
        CHECK(!mem::mapping::find(pr.r.span.start + page));

        mem::mapping::depopulate({pr.r.span.start, pr.r.span.end});
        mem::vspace::release(pr.r);
    }

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

    section("to_phys translates a mapped address and refuses an empty one");
    {
        scratch s(4 * page);
        mem::frames::phys_addr pa = mem::frames::alloc();
        CHECK(map::attach(s.range(0, page), pa, mem::perm_rw));
        CHECK(map::to_phys(s.start()) == pa);
        CHECK(map::to_phys(s.start() + 100) == pa + 100);
        CHECK(map::to_phys(s.start() + page) == mem::frames::no_memory);
        CHECK(map::to_phys(mem::frames::to_linear(pa)) == pa);
        map::detach(s.range(0, page));
        mem::frames::free(pa);
    }

    section("is_contiguous tells one run of memory from a seam");
    {
        // The same frame at two neighbouring addresses: bytes flow across the
        // boundary virtually, but physically the second page starts over.
        scratch s(4 * page);
        mem::frames::phys_addr pa = mem::frames::alloc();
        CHECK(map::attach(s.range(0, page), pa, mem::perm_rw));
        CHECK(map::attach(s.range(page, page), pa, mem::perm_rw));
        auto *p = reinterpret_cast<const void *>(s.start());
        CHECK(map::is_contiguous(p, page));
        CHECK(!map::is_contiguous(p, 2 * page));
        CHECK(map::is_contiguous(mem::frames::to_linear(pa), page));
        map::detach(s.range(0, 2 * page));
        mem::frames::free(pa);
    }

    section("the entry records reads and writes, and clearing starts over");
    {
        scratch s(4 * page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        mem::range v = s.range(0, page);

        map::clear_accessed(v);
        map::clear_dirty(v);
        // A cpu holding the old entry never sets the bits again.
        map::flush_all();
        CHECK(!map::accessed(v));
        if (map::tracks_writes) {
            CHECK(!map::dirty(v));
        }

        (void)*reinterpret_cast<volatile char *>(s.start());
        CHECK(map::accessed(v));
        if (map::tracks_writes) {
            CHECK(!map::dirty(v));
        }

        *reinterpret_cast<volatile char *>(s.start()) = 1;
        CHECK(map::dirty(v));
        map::depopulate(v);
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

/* early ------------------------------------------------------------------- */

void early_functional()
{
    group("early");

    section("an early object round-trips, before the heap and after");
    {
        CHECK(mem::early::takes(64, 8));
        void *p = mem::early::alloc(64, 8);
        CHECK(p != nullptr);
        CHECK(mem::early::owns(p));
        CHECK(!mem::heap::owns(p));
        CHECK(mem::early::size_of(p) == 64);
        memset(p, 0x77, 64);
        CHECK(static_cast<unsigned char *>(p)[63] == 0x77);
        mem::early::free(p);
    }

    section("alignment is honoured");
    {
        void *p = mem::early::alloc(100, 256);
        CHECK((reinterpret_cast<uintptr_t>(p) & 255) == 0);
        mem::early::free(p);
    }

    section("as many pages as boot asks for, all given back");
    {
        // One object per page, live at once: boot holds one of these for every
        // cpu it brings up, and the count is not something to guess at.
        const int n = 512;
        const size_t big = page - 64;
        std::vector<void *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = mem::early::alloc(big, 8);
            CHECK(p[i] != nullptr);
            memset(p[i], i & 0xff, big);
        }
        bool own = true, kept = true;
        for (int i = 0; i < n; i++) {
            own = own && mem::early::owns(p[i]);
            kept = kept && static_cast<unsigned char *>(p[i])[big - 1] == (i & 0xff);
        }
        CHECK(own);
        CHECK(kept);
        for (int i = 0; i < n; i++) {
            mem::early::free(p[i]);
        }
        void *q = mem::early::alloc(64, 8);
        CHECK(q != nullptr);
        CHECK(mem::early::owns(q));
        mem::early::free(q);
    }

    section("what no page can hold gets frames of its own");
    {
        CHECK(!mem::early::takes(3 * page, 64));
        auto *p = static_cast<char *>(mem::early::alloc(3 * page, 64));
        CHECK(p != nullptr);
        CHECK(mem::early::owns(p));
        CHECK(mem::early::size_of(p) >= 3 * page);
        memset(p, 0x2f, 3 * page);
        CHECK(static_cast<unsigned char>(p[3 * page - 1]) == 0x2f);
        mem::early::free(p);
    }
}

/* phys -------------------------------------------------------------------- */

void phys_functional()
{
    group("phys");

    section("map_phys hands back a pointer to the frames it was given");
    {
        mem::frames::phys_addr pa = mem::frames::alloc();
        auto *v = static_cast<char *>(mem::map_phys(pa, page));
        CHECK(v != nullptr);
        v[0] = 0x5c;
        v[page - 1] = 0x5d;
        auto *l = static_cast<char *>(mem::frames::to_linear(pa));
        CHECK(l[0] == 0x5c);
        CHECK(static_cast<unsigned char>(l[page - 1]) == 0x5d);
        mem::frames::free(pa);
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

    section("the heap answers for what it owns");
    {
        CHECK(mem::heap::takes(16, 16));
        CHECK(mem::heap::takes(1ul << 20, 4096));
        CHECK(!mem::heap::takes(100, 4ul << 20));

        void *p = mem::heap::alloc(100, 16);
        CHECK(p != nullptr);
        CHECK(mem::heap::owns(p));
        CHECK(mem::heap::size_of(p) >= 100);
        mem::heap::free(p);

        // The sized form, which is what operator delete supplies.
        void *q = mem::heap::alloc(64, 16);
        CHECK(q != nullptr);
        mem::heap::free(q, 64);

        // A mapping has a reservation of its own too, and the heap must not
        // mistake it for one of its large allocations.
        void *m = mmap(nullptr, huge, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED);
        CHECK(!mem::heap::owns(m));
        CHECK(mem::heap::size_of(m) == 0);
        CHECK(munmap(m, huge) == 0);
    }

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
        // Enough of the heap to measure, and not so much that the objects take
        // longer to make than the giving back takes to show.
        size_t asked = std::min<size_t>(before / 2, 4ul << 30);
        size_t n = asked / size;
        std::vector<void *> p(n);
        for (size_t i = 0; i < n; i++) {
            p[i] = malloc(size);
        }
        for (size_t i = 0; i < n; i++) {
            free(p[i]);
        }
        size_t held = before - mem::frames::free_bytes();
        CHECK(held > asked / 2);      // it really is holding it

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

/* page cache -------------------------------------------------------------- */

/*
 * A backend with nothing behind it: every byte is a function of where it is, so
 * an object of any size costs nothing to serve and every byte read can be
 * checked against what it should have been. Synchronous, since what is being
 * tested here is the cache and not a driver.
 */
struct pattern_store : mem::store {
    explicit pattern_store(uint64_t bytes) : _bytes(bytes) {}

    uint64_t size() override { return _bytes; }

    mem::io *read(void *buf, uint64_t off, size_t bytes) override
    {
        fill(static_cast<uint8_t *>(buf), off, bytes);
        reads += bytes;
        return done_with(bytes);
    }

    mem::io *write(const void *buf, uint64_t off, size_t bytes) override
    {
        // Only where a test is going to look. Where the hardware keeps no
        // dirty bit every eviction writes back, and an object larger than
        // memory would put every byte of itself in here.
        if (record) {
            auto *w = static_cast<const uint64_t *>(buf);
            for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
                written[off + i * sizeof(uint64_t)] = w[i];
            }
        }
        writes += bytes;
        return done_with(bytes);
    }

    bool done(mem::io *) override { return true; }

    int64_t wait(mem::io *req) override
    {
        auto *r = reinterpret_cast<int64_t *>(req);
        int64_t n = *r;
        delete r;
        return n;
    }

    // Every aligned word is its own offset, so a byte anywhere says where it
    // came from and a transfer of any length can be checked.
    static uint64_t word(uint64_t off) { return off; }

    static void fill(uint8_t *p, uint64_t off, size_t n)
    {
        size_t whole = n & ~size_t(7);
        auto *w = reinterpret_cast<uint64_t *>(p);
        for (size_t i = 0; i < whole / sizeof(uint64_t); i++) {
            w[i] = off + i * sizeof(uint64_t);
        }
        for (size_t i = whole; i < n; i++) {
            p[i] = uint8_t((off + (i & ~size_t(7))) >> (8 * (i & 7)));
        }
    }

    uint64_t _bytes;
    bool record = false;
    std::atomic<size_t> reads{0};
    std::atomic<size_t> writes{0};
    std::map<uint64_t, uint64_t> written;   // only where a test wrote something

private:
    mem::io *done_with(size_t bytes)
    {
        return reinterpret_cast<mem::io *>(new int64_t(bytes));
    }
};

// Reads that ask for more than a page at a time, to check that a buffer is
// whatever the policy says it is rather than always one page.
size_t fault_64k(void *, uint64_t) { return 64 * 1024; }

// Three pages and a bit: bigger than a couple of pages, and no whole number of
// them, so consecutive buffers begin and end part-way through one.
const uint64_t ragged_span = 3 * 4096 + 1000;
size_t fault_ragged(void *, uint64_t) { return ragged_span; }

// Megabytes and misaligned: the analytics-page shape, with whole 2 MiB spans
// inside every buffer.
const uint64_t big_span = (5ull << 20) + 12345;
size_t fault_big(void *, uint64_t) { return big_span; }

size_t fault_huge(void *, uint64_t) { return huge; }

/* store ------------------------------------------------------------------- */

void store_functional()
{
    group("store");

    section("a store moves bytes both ways and says when it is done");
    {
        pattern_store s(1ul << 20);
        s.record = true;
        std::vector<uint8_t> buf(3 * page);

        CHECK(s.read_now(buf.data(), 8192, buf.size()) == (int64_t)buf.size());
        bool ok = true;
        for (size_t i = 0; i + 8 <= buf.size(); i += 8) {
            uint64_t w;
            memcpy(&w, buf.data() + i, 8);
            ok = ok && w == pattern_store::word(8192 + i);
        }
        CHECK(ok);

        uint64_t magic = 0x1122334455667788ull;
        memcpy(buf.data(), &magic, 8);
        CHECK(s.write_now(buf.data(), 4096, 8) == 8);
        CHECK(s.written[4096] == magic);

        // The split form: start, ask, wait.
        mem::io *req = s.read(buf.data(), 0, page);
        CHECK(req != nullptr);
        CHECK(s.done(req));
        CHECK(s.wait(req) == (int64_t)page);
    }
}

/*
 * A policy that records everything the cache tells it, so every hook and every
 * buffer accessor is checked from the policy's side of the contract. Victims
 * are kept on a stack threaded through policy_data.
 */
struct spy_state {
    uint64_t created_size = 0;
    std::atomic<int> faults{0}, evicted{0};
    std::atomic<bool> accessors_ok{true};
    mutex lock;
    mem::pagecache::buffer *top = nullptr;
};

spy_state g_spy;
std::atomic<bool> g_spy_destroyed{false};

void *spy_create(uint64_t store_size)
{
    g_spy.created_size = store_size;
    g_spy.faults = 0;
    g_spy.evicted = 0;
    g_spy.accessors_ok = true;
    g_spy.top = nullptr;
    g_spy_destroyed = false;
    return &g_spy;
}

void spy_destroy(void *p)
{
    g_spy_destroyed = p == &g_spy;
}

void spy_on_fault(void *p, mem::pagecache::buffer &b)
{
    namespace pc = mem::pagecache;
    auto *st = static_cast<spy_state *>(p);
    st->faults.fetch_add(1);
    bool ok = pc::size(b) > 0 && pc::size(b) <= ragged_span &&
              pc::offset(b) + pc::size(b) <= st->created_size &&
              pc::data(b) != nullptr;
    // The contents are there before the policy hears of the buffer.
    if (ok && pc::size(b) >= 8 && pc::offset(b) % 8 == 0) {
        ok = *static_cast<uint64_t *>(pc::data(b)) == pattern_store::word(pc::offset(b));
    }
    if (!ok) {
        st->accessors_ok = false;
    }
    WITH_LOCK(st->lock) {
        *static_cast<pc::buffer **>(pc::policy_data(b)) = st->top;
        st->top = &b;
    }
}

void spy_evict(void *p, size_t bytes, mem::pagecache::buffer_list &victims)
{
    namespace pc = mem::pagecache;
    auto *st = static_cast<spy_state *>(p);
    size_t got = 0;
    WITH_LOCK(st->lock) {
        while (st->top && got < bytes && !victims.full()) {
            pc::buffer *b = st->top;
            st->top = *static_cast<pc::buffer **>(pc::policy_data(*b));
            victims.add(b);
            got += pc::size(*b);
        }
    }
}

void spy_on_evicted(void *p, mem::pagecache::buffer &)
{
    static_cast<spy_state *>(p)->evicted.fetch_add(1);
}

bool spy_is_dirty(void *, mem::pagecache::buffer &)
{
    return false;
}

const mem::pagecache::policy spy_policy = {
    .bytes_per_buffer = sizeof(void *),
    .create = spy_create,
    .destroy = spy_destroy,
    .fault_size = fault_ragged,
    .prefetch = nullptr,
    .evict = spy_evict,
    .on_fault = spy_on_fault,
    .on_evicted = spy_on_evicted,
    .is_dirty = spy_is_dirty,
};

void pagecache_functional()
{
    group("pagecache");
    namespace pc = mem::pagecache;

    section("a page is read in when it is touched, and not before");
    {
        pattern_store s(16 * huge);
        void *m = pc::map(s);
        CHECK(m != nullptr);
        CHECK(!pc::resident(m));
        CHECK(s.reads.load() == 0);

        auto *w = static_cast<volatile uint64_t *>(m);
        CHECK(w[0] == pattern_store::word(0));
        CHECK(pc::resident(m));
        CHECK(s.reads.load() == page);

        // Somewhere else entirely, which the first fault cannot have covered.
        auto *far = reinterpret_cast<volatile uint64_t *>(
            static_cast<char *>(m) + 8 * huge);
        CHECK(!pc::resident(const_cast<uint64_t *>(far)));
        CHECK(far[0] == pattern_store::word(8 * huge));
        CHECK(s.reads.load() == 2 * page);
        pc::unmap(m);
    }

    section("a buffer is as big as the policy asks for");
    {
        pattern_store s(16 * huge);
        pc::policy p = pc::defaults();
        p.fault_size = fault_64k;

        void *m = pc::map(s, p);
        CHECK(m != nullptr);
        auto *b = static_cast<char *>(m);
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + 8 * page) ==
              pattern_store::word(8 * page));

        // One transfer, and the fifteen pages around it are there without
        // another fault between them.
        CHECK(s.reads.load() == 64 * 1024);
        for (int i = 0; i < 16; i++) {
            CHECK(pc::resident(b + i * page));
        }
        CHECK(!pc::resident(b + 16 * page));
        pc::unmap(m);
    }

    section("a buffer holds every page it touches, shared edges included");
    {
        /*
         * Buffers of an awkward size, which is the case this is all for: an
         * analytics page is whatever it is, and consecutive ones land wherever
         * the one before them ended.
         *
         *   bytes 0     13288       26576       39864       53152
         *         |  b0   |    b1     |    b2     |    b3     |
         *   pages 0  1  2  3  4  5  6  7  8  9 10 11 12
         *
         * b2 is [26576, 39864). Pages 7 and 8 are inside it; pages 6 and 9 it
         * shares with the buffer either side, and it holds those whole.
         */
        const uint64_t span = ragged_span;
        pattern_store s(16 * huge);
        pc::policy p = pc::defaults();
        p.fault_size = fault_ragged;

        void *m = pc::map(s, p);
        CHECK(m != nullptr);
        auto *b = static_cast<char *>(m);

        const uint64_t b2 = 2 * span;
        const uint64_t lo = b2 / page;                       // 6, shared below
        const uint64_t hi = (3 * span - 1) / page;           // 9, shared above
        CHECK(lo * page < b2);                               // it really is shared
        CHECK((hi + 1) * page > 3 * span);

        // One touch in the middle of it. A fault names the page it happened
        // in, not the byte, so this has to be somewhere the page it lands in
        // begins inside b2 -- which is what "inner" means here.
        const uint64_t mid = (b2 + span / 2) & ~uint64_t(7);
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + mid) ==
              pattern_store::word(mid));

        // Every page it touches came in, the two it only partly owns included,
        // and nothing beyond them did.
        for (uint64_t i = lo; i <= hi; i++) {
            CHECK(pc::resident(b + i * page));
        }
        CHECK(!pc::resident(b + (lo - 1) * page));
        CHECK(!pc::resident(b + (hi + 1) * page));
        CHECK(s.reads.load() == (hi - lo + 1) * page);

        // The bytes of b2 that fell in those shared pages are readable now,
        // although the buffers they belong to have never been faulted.
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + lo * page) ==
              pattern_store::word(lo * page));
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + hi * page) ==
              pattern_store::word(hi * page));

        // Its neighbour now finds one of its own pages taken. It stops at the
        // edge rather than fighting for it, so it comes up short by the part
        // of itself that fell in page "lo".
        size_t before = s.reads.load();
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + span) ==
              pattern_store::word(span));
        CHECK(s.reads.load() - before == (lo - span / page) * page);
        CHECK(pc::resident(b + span));
        // b1 starts part-way through page 3, and that page came in with it and
        // not with b0, which has still not been read at all.
        CHECK(pc::resident(b + (span / page) * page));
        CHECK(!pc::resident(b + (span / page - 1) * page));

        // And every byte of the four of them reads as the object says, across
        // the seams and through the pages two buffers had a claim on.
        bool ok = true;
        for (uint64_t off = 0; off < 4 * span; off += sizeof(uint64_t)) {
            ok &= *reinterpret_cast<volatile uint64_t *>(b + off) ==
                  pattern_store::word(off);
        }
        CHECK(ok);
        pc::unmap(m);
    }

    section("a page two buffers fall in goes to the one that was reached for");
    {
        // Page 3 holds the end of b0 and the start of b1. Which of them it
        // comes in with is decided by the byte that faulted, so the same page
        // goes either way depending on what was touched.
        const uint64_t span = ragged_span;
        const uint64_t seam = span / page;               // 3
        const uint64_t here = seam * page;               // 12288, still b0

        {
            pattern_store s(16 * huge);
            pc::policy p = pc::defaults();
            p.fault_size = fault_ragged;
            void *m = pc::map(s, p);
            auto *b = static_cast<char *>(m);

            CHECK(*reinterpret_cast<volatile uint64_t *>(b + here) ==
                  pattern_store::word(here));
            // b0 is [0, span): pages 0 to 3.
            CHECK(pc::resident(b));
            CHECK(pc::resident(b + here));
            CHECK(!pc::resident(b + here + page));
            CHECK(s.reads.load() == (seam + 1) * page);
            pc::unmap(m);
        }
        {
            pattern_store s(16 * huge);
            pc::policy p = pc::defaults();
            p.fault_size = fault_ragged;
            void *m = pc::map(s, p);
            auto *b = static_cast<char *>(m);

            // The same page, a few hundred bytes further along, where b1
            // begins. It comes in with b1 instead, and b0 is untouched.
            CHECK(*reinterpret_cast<volatile uint64_t *>(b + span) ==
                  pattern_store::word(span));
            CHECK(!pc::resident(b));
            CHECK(pc::resident(b + here));
            CHECK(pc::resident(b + here + page));
            // b1 is [span, 2 * span): pages 3 to 6.
            const uint64_t last = (2 * span - 1) / page;
            CHECK(s.reads.load() == (last + 1 - seam) * page);
            pc::unmap(m);
        }
    }

    section("an object that ends part-way through a page");
    {
        const uint64_t odd = 4 * huge + 1234;
        pattern_store s(odd);
        void *m = pc::map(s);
        CHECK(m != nullptr);
        auto *b = static_cast<char *>(m);

        const uint64_t last = (odd - 8) & ~uint64_t(7);
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + last) ==
              pattern_store::word(last));
        // The store has no more to give, and what is left of the page it ended
        // in is reachable, so it has to be something. It is zero.
        CHECK(b[odd] == 0);
        CHECK(b[odd + 100] == 0);
        pc::unmap(m);
    }

    section("fetch brings in a range in one go");
    {
        pattern_store s(16 * huge);
        void *m = pc::map(s);
        CHECK(m != nullptr);
        CHECK(pc::fetch(m, 32 * page) == 32 * page);
        CHECK(s.reads.load() == 32 * page);

        auto *w = static_cast<volatile uint64_t *>(m);
        bool ok = true;
        for (size_t i = 0; i < 32 * page / sizeof(uint64_t); i++) {
            ok &= w[i] == pattern_store::word(i * sizeof(uint64_t));
        }
        CHECK(ok);
        // Nothing more was read: fetch left the pages mapped, not just fetched.
        CHECK(s.reads.load() == 32 * page);
        pc::unmap(m);
    }

    section("what is written goes back on sync, and again on unmap");
    {
        pattern_store s(16 * huge);
        s.record = true;
        void *m = pc::map(s);
        CHECK(m != nullptr);

        auto *w = static_cast<uint64_t *>(m);
        w[0] = 0xfeed;
        CHECK(pc::sync(m, page) == (int64_t)page);
        CHECK(s.written[0] == 0xfeed);
        CHECK(s.writes.load() == page);

        // Nothing has changed since, so there is nothing to write -- where the
        // hardware says so. Where it does not, everything reads as written to.
        if (mem::mapping::tracks_writes) {
            CHECK(pc::sync(m, page) == 0);
            CHECK(s.writes.load() == page);
        }

        auto *later = reinterpret_cast<uint64_t *>(static_cast<char *>(m) + 4 * page);
        *later = 0xbeef;
        pc::unmap(m);
        CHECK(s.written[4 * page] == 0xbeef);
    }

    section("a cache with a limit stays inside it");
    {
        const size_t cap = 64 << 20;
        pattern_store s(8ull << 30);
        void *m = pc::map(s, pc::defaults(), cap);
        CHECK(m != nullptr);

        // Measured from the mapped cache: the policy sizes its queues at
        // create, from the store and the memory, and the limit is on buffers.
        size_t before = mem::frames::free_bytes();
        auto *b = static_cast<char *>(m);
        bool ok = true;
        size_t worst = 0;
        for (uint64_t off = 0; off < 8 * size_t(cap); off += page) {
            ok &= *reinterpret_cast<volatile uint64_t *>(b + off) ==
                  pattern_store::word(off);
            size_t held = before - mem::frames::free_bytes();
            worst = std::max(worst, held);
        }
        CHECK(ok);
        // Eight times its limit went through it, and it never held much more
        // than the limit at once. The slack is the page tables for the range
        // and what the frame allocator keeps in its per-cpu caches.
        CHECK(worst < cap + (cap / 2));
        printf("      a %zu MiB limit held at most %zu MiB\n", cap >> 20, worst >> 20);
        pc::unmap(m);
    }

    section("a big misaligned buffer takes huge frames where it can");
    {
        pattern_store s(1ull << 30);
        pc::policy p = pc::defaults();
        p.fault_size = fault_big;
        void *m = pc::map(s, p);
        CHECK(m != nullptr);
        auto *b = static_cast<char *>(m);

        // One touch in the second tile brings its whole ~5 MiB in.
        const uint64_t off = (big_span + 3 * page) & ~uint64_t(7);
        CHECK(*reinterpret_cast<volatile uint64_t *>(b + off) ==
              pattern_store::word(off));
        size_t foot = ((2 * big_span - 1) / page - big_span / page + 1) * page;
        CHECK(s.reads.load() == foot);

        // The aligned 2 MiB spans inside it are single level-1 leaves; the
        // ragged lead is small entries.
        uintptr_t base = reinterpret_cast<uintptr_t>(b);
        auto h = map::find(base + align_up(big_span, uint64_t(huge)));
        CHECK(bool(h));
        CHECK(h.level() == 1);
        auto l = map::find(base + align_down(big_span, uint64_t(page)));
        CHECK(bool(l));
        CHECK(l.level() == 0);

        // Every byte of it, seams and huge interiors alike.
        bool ok = true;
        for (uint64_t o = big_span; o < 2 * big_span; o += 8) {
            ok &= *reinterpret_cast<volatile uint64_t *>(b + (o & ~uint64_t(7))) ==
                  pattern_store::word(o & ~uint64_t(7));
        }
        CHECK(ok);
        pc::unmap(m);
    }

    section("big buffers cycle through a limit");
    {
        const size_t cap = 64 << 20;
        pattern_store s(1ull << 30);
        pc::policy p = pc::defaults();
        p.fault_size = fault_big;
        size_t before = mem::frames::free_bytes();
        void *m = pc::map(s, p, cap);
        CHECK(m != nullptr);
        auto *b = static_cast<char *>(m);

        // Eight limits' worth of object, one probe per tile.
        bool ok = true;
        size_t worst = 0;
        for (uint64_t off = 0; off + 8 <= 8 * uint64_t(cap); off += big_span) {
            uint64_t o = (off + big_span / 2) & ~uint64_t(7);
            ok &= *reinterpret_cast<volatile uint64_t *>(b + o) ==
                  pattern_store::word(o);
            worst = std::max(worst, before - mem::frames::free_bytes());
        }
        CHECK(ok);
        CHECK(worst < cap + (cap / 2));
        printf("      %zu MiB of %zu KiB buffers through a %zu MiB limit, held %zu MiB\n",
               size_t(8) * (cap >> 20), size_t(big_span >> 10), cap >> 20, worst >> 20);
        pc::unmap(m);
    }

    section("a policy of the application's own sees the life of every buffer");
    {
        const uint64_t bytes = 64ul << 20;
        pattern_store s(bytes);
        s.record = true;
        void *m = pc::map(s, spy_policy, 8ul << 20);
        CHECK(m != nullptr);
        CHECK(g_spy.created_size == bytes);

        auto *b = static_cast<char *>(m);
        b[0] = 0x55;                    // written, so write-back would show
        bool ok = true;
        for (uint64_t off = page; off < bytes; off += ragged_span) {
            uint64_t o = off & ~uint64_t(7);
            ok &= *reinterpret_cast<volatile uint64_t *>(b + o) ==
                  pattern_store::word(o);
        }
        CHECK(ok);
        CHECK(g_spy.accessors_ok.load());
        CHECK(g_spy.faults.load() >= int(bytes / ragged_span) / 2);
        CHECK(g_spy.evicted.load() > 0);

        pc::unmap(m);
        // unmap drains through the policy, so everything it was ever handed
        // has come back, and its is_dirty verdict held: nothing was written.
        CHECK(g_spy.evicted.load() == g_spy.faults.load());
        CHECK(g_spy_destroyed.load());
        CHECK(s.writes.load() == 0);
    }

    section("two caches live side by side and die separately");
    {
        pattern_store s1(4 * huge), s2(4 * huge);
        char *m1 = static_cast<char *>(pc::map(s1));
        char *m2 = static_cast<char *>(pc::map(s2));
        CHECK(m1 != nullptr);
        CHECK(m2 != nullptr);
        CHECK(m1 != m2);
        CHECK(*reinterpret_cast<volatile uint64_t *>(m1) == pattern_store::word(0));
        CHECK(*reinterpret_cast<volatile uint64_t *>(m2 + 8) == pattern_store::word(8));
        pc::unmap(m1);
        CHECK(*reinterpret_cast<volatile uint64_t *>(m2 + 16) == pattern_store::word(16));
        CHECK(pc::resident(m2));
        pc::unmap(m2);
    }

    section("ragged buffers survive eviction, from many threads");
    {
        // Buffers that share edge pages, a limit small enough that eviction
        // runs the whole time, and every cpu faulting at once: the edge
        // handover between a leaving buffer and an arriving neighbour has to
        // hold under all of it.
        pattern_store s(1ull << 30);
        pc::policy p = pc::defaults();
        p.fault_size = fault_ragged;
        void *m = pc::map(s, p, 32 << 20);
        CHECK(m != nullptr);

        auto *b = static_cast<char *>(m);
        std::atomic<bool> ok{true};
        std::atomic<uint64_t> bad_off{0}, bad_val{0}, bad_again{0};
        parallel(n_cpus(), [&](unsigned t) {
            uint64_t seed = 0x9e3779b9u * (t + 1);
            for (int i = 0; i < 20000; i++) {
                seed = seed * 6364136223846793005ull + 1;
                uint64_t off = (seed >> 16) % ((1ull << 30) - 8) & ~uint64_t(7);
                uint64_t v = *reinterpret_cast<volatile uint64_t *>(b + off);
                if (v != pattern_store::word(off)) {
                    if (ok.exchange(false)) {
                        bad_off = off;
                        bad_val = v;
                        bad_again = *reinterpret_cast<volatile uint64_t *>(b + off);
                    }
                }
            }
        });
        if (!ok) {
            uint64_t o = bad_off.load(), v = bad_val.load();
            printf("      at %#lx read %#lx (delta %ld, page %+ld, spans %+ld) "
                   "reread %#lx\n",
                   o, v, (long)(v - o), (long)(v - o) / (long)page,
                   (long)(v - o) / (long)ragged_span, bad_again.load());
        }
        CHECK(ok);
        pc::unmap(m);
    }

    section("a working set bigger than the cache is served and served correctly");
    {
        // Twice what the cache may hold, so that it has to give pages back to
        // reach the end of the object, and has to fault back what it gave up.
        // What it may hold is all of free memory, until there is more of that
        // than a pass over twice it is worth spending: past that, a limit puts
        // the same pressure on the cache at a fixed cost.
        const uint64_t roof = 16ull << 30;
        const uint64_t room = mem::frames::free_bytes();
        const size_t limit = room > roof ? roof : 0;
        const uint64_t bytes = (2 * (limit ? limit : room)) & ~(huge - 1);
        pattern_store s(bytes);
        // 2 MiB at a time where the object is big, so that a pass costs what
        // the store can move and not one fault per page of it.
        pc::policy p = pc::defaults();
        if (bytes > (8ull << 30)) {
            p.fault_size = fault_huge;
        }
        auto t0 = clk::now();
        void *m = pc::map(s, p, limit);
        CHECK(m != nullptr);

        auto *b = static_cast<char *>(m);
        bool ok = true;
        for (uint64_t off = 0; off < bytes; off += page) {
            ok &= *reinterpret_cast<volatile uint64_t *>(b + off) ==
                  pattern_store::word(off);
        }
        CHECK(ok);
        CHECK(s.reads.load() >= bytes);

        // The start of the object is what was faulted longest ago, so going
        // back to it reads it in again -- which is eviction, seen from outside.
        size_t after_first_pass = s.reads.load();
        for (uint64_t off = 0; off < bytes / 8; off += page) {
            ok &= *reinterpret_cast<volatile uint64_t *>(b + off) ==
                  pattern_store::word(off);
        }
        CHECK(ok);
        // Most of that eighth had to come back from the store: it is the part
        // that was faulted longest ago, and so the part a fifo gives up first.
        CHECK(s.reads.load() - after_first_pass > bytes / 8 / 2);
        printf("      %zu MiB of object through %zu MiB, %zu MiB read in %.1f s\n",
               (size_t)(bytes >> 20),
               (limit ? limit : mem::frames::total_available_bytes()) >> 20,
               s.reads.load() >> 20, since(t0));
        pc::unmap(m);
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
    early_functional();
    phys_functional();
    heap_functional();
    store_functional();
    pagecache_functional();

    return summary("MEMORY PRIMITIVE");
}
