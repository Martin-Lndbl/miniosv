/*
 * Memory subsystem: correctness and performance, per layer.
 *
 * The test application is linked into the kernel, so it calls the memory
 * primitives directly rather than through libc. Sections follow the layers in
 * PLAN_mem.md; until those layers exist they call today's equivalents, and the
 * numbers printed here are the baseline the rewrite is measured against.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <osv/contiguous_alloc.hh>
#include <osv/mempool.hh>
#include <osv/mmu.hh>
#include <osv/mem/frames.hh>
#include <osv/pagealloc.hh>
#include <osv/sched.hh>

namespace {

std::atomic<int> g_checks{0};
std::atomic<int> g_fails{0};
const char *g_section = "";

#define CHECK(cond) do { \
        g_checks.fetch_add(1); \
        if (!(cond)) { \
            g_fails.fetch_add(1); \
            printf("    FAIL [%s] %s:%d: %s\n", g_section, __FILE__, __LINE__, #cond); \
        } \
    } while (0)

void group(const char *s) { printf("\n== %s ==\n", s); }
void section(const char *s) { g_section = s; printf("  - %s\n", s); }

using clk = std::chrono::steady_clock;

double since(clk::time_point t0)
{
    return std::chrono::duration<double>(clk::now() - t0).count();
}

// clang knows malloc/free and will delete a pair whose result is unused.
void escape(void *p)
{
    asm volatile("" : : "r,m"(p) : "memory");
}

void report_ns(const char *what, double s, double n)
{
    printf("    %-46s %9.1f ns/op\n", what, s * 1e9 / n);
}

// ops is the total across all threads; the per-thread cost is what shows
// whether the work actually got faster or just got shared out.
void report_scale(const char *what, unsigned threads, double ops, double s)
{
    printf("    %-32s %3u thr %8.2f Mops/s %9.1f ns/op\n",
           what, threads, ops / s / 1e6, s * 1e9 * threads / ops);
}

// For measurements that are serialised today, keep the total work constant so
// the run does not take longer and longer as threads are added.
int share(int total, unsigned threads, int least = 4)
{
    return std::max(least, total / static_cast<int>(threads));
}

unsigned n_cpus()
{
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

// Runs fn(i) on `threads` threads started together; returns the wall time.
// Pins each thread to a cpu.
template <typename F>
double parallel(unsigned threads, F fn)
{
    std::vector<std::thread> ts;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    for (unsigned i = 0; i < threads; i++) {
        ts.emplace_back([&, i] {
            sched::thread::pin(sched::cpus[i % sched::cpus.size()]);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            fn(i);
        });
    }
    while (ready.load() != threads) {
        std::this_thread::yield();
    }
    auto t0 = clk::now();
    go.store(true, std::memory_order_release);
    for (auto &t : ts) {
        t.join();
    }
    double s = since(t0);
    return s;
}

/* frames ------------------------------------------------------------------ */

void frames_functional()
{
    group("frames");

    section("4 KiB frames are distinct, aligned and writable");
    {
        const int n = 64;
        void *p[n];
        for (int i = 0; i < n; i++) {
            p[i] = memory::alloc_page();
            CHECK(p[i] != nullptr);
            CHECK(mmu::is_page_aligned(p[i]));
            memset(p[i], 0xa5, mmu::page_size);
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                CHECK(p[i] != p[j]);
            }
        }
        for (int i = 0; i < n; i++) {
            CHECK(static_cast<unsigned char *>(p[i])[mmu::page_size - 1] == 0xa5);
            memory::free_page(p[i]);
        }
    }

    section("frames are in the linear map and round-trip virt<->phys");
    {
        void *p = memory::alloc_page();
        CHECK(mmu::is_linear_mapped(p, mmu::page_size));
        mmu::phys pa = mmu::virt_to_phys(p);
        CHECK(mmu::phys_to_virt(pa) == p);
        CHECK((pa & (mmu::page_size - 1)) == 0);
        memory::free_page(p);
    }

    section("2 MiB frames are 2 MiB aligned");
    {
        void *h = memory::alloc_huge_page(mmu::huge_page_size);
        CHECK(h != nullptr);
        if (h) {
            CHECK((reinterpret_cast<uintptr_t>(h) & (mmu::huge_page_size - 1)) == 0);
            memset(h, 0x5a, mmu::huge_page_size);
            memory::free_huge_page(h, mmu::huge_page_size);
        }
    }

    section("contiguous allocation really is contiguous");
    {
        const size_t sizes[] = {1ul << 20, 8ul << 20};
        for (size_t size : sizes) {
            void *p = memory::alloc_phys_contiguous_aligned(size, mmu::page_size);
            CHECK(p != nullptr);
            if (!p) {
                continue;
            }
            mmu::phys base = mmu::virt_to_phys(p);
            bool ok = true;
            for (size_t off = 0; off < size; off += mmu::page_size) {
                ok = ok && mmu::virt_to_phys(static_cast<char *>(p) + off) == base + off;
            }
            CHECK(ok);
            memory::free_phys_contiguous_aligned(p, size);
        }
    }

    section("free memory returns to its starting value");
    {
        // The per-CPU page pools sit between alloc_page and the accounting, so
        // the counter lags by up to a pool's worth. Exact accounting is one of
        // the things the rewrite should buy; record the drift for now.
        const size_t slack = 64ul << 20;
        size_t before = memory::stats::free();
        const int n = 4096;
        std::vector<void *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = memory::alloc_page();
        }
        size_t during = memory::stats::free();
        CHECK(during <= before);
        for (int i = 0; i < n; i++) {
            memory::free_page(p[i]);
        }
        size_t after = memory::stats::free();
        CHECK(after >= during);
        CHECK(after + slack >= before);
        printf("      total %zu MiB, free %zu MiB, drift after %d pages: %ld KiB\n",
               memory::stats::total() >> 20, after >> 20, n,
               (static_cast<long>(before) - static_cast<long>(after)) >> 10);
    }
}

void frames_perf()
{
    group("frames - performance");

    const int batch = 512;
    const int rounds = 100;
    std::vector<void *> p(batch);

    section("order 0");
    {
        // Warm the per-CPU pools so the first measurement is not the only one
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

    section("order 0, through mem::frames directly");
    {
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned) {
                std::vector<void *> q(batch);
                for (int r = 0; r < rounds; r++) {
                    for (int i = 0; i < batch; i++) {
                        q[i] = mem::frames::to_linear(mem::frames::alloc());
                        escape(q[i]);
                    }
                    for (int i = 0; i < batch; i++) {
                        mem::frames::free(mem::frames::from_linear(q[i]));
                    }
                }
            });
            report_scale("frames::alloc + free", t, 2.0 * batch * rounds * t, s);
        }
    }

    section("cpu spread");
    {
        unsigned t = n_cpus();
        std::vector<unsigned> seen(t, 0u);
        parallel(t, [&](unsigned id) {
            for (int i = 0; i < 1000; i++) {
                mem::frames::free(mem::frames::alloc());
            }
            seen[id] = sched::cpu::current() ? sched::cpu::current()->id : 9999;
        });
        unsigned distinct = 0;
        for (unsigned i = 0; i < t; i++) {
            bool dup = false;
            for (unsigned j = 0; j < i; j++) {
                dup = dup || seen[j] == seen[i];
            }
            distinct += !dup;
        }
        printf("    %-46s %9u of %u\n", "distinct cpus running the threads", distinct, t);
    }

    section("order 9");
    {
        const int total = 512;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    void *h = memory::alloc_huge_page(mmu::huge_page_size);
                    if (!h) {
                        break;
                    }
                    escape(h);
                    memory::free_huge_page(h, mmu::huge_page_size);
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
                void *q = memory::alloc_phys_contiguous_aligned(size, mmu::page_size);
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

    section("reserving consumes no physical memory");
    {
        const size_t size = 64ul << 20;
        size_t before = memory::stats::free();
        void *p = mmu::map_anon(nullptr, size, 0, mmu::perm_rw);
        CHECK(p != nullptr);
        CHECK(before - memory::stats::free() < size / 8);
        CHECK(!mmu::munmap(p, size).bad());
    }

    section("populate maps immediately, unmap returns the memory");
    {
        const size_t size = 8ul << 20;
        size_t before = memory::stats::free();
        void *p = mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_rw);
        CHECK(p != nullptr);
        CHECK(before - memory::stats::free() >= size / 2);
        CHECK(mmu::ismapped(p, size));
        CHECK(!mmu::munmap(p, size).bad());
        CHECK(memory::stats::free() + (size / 4) >= before);
    }

    section("reservations do not overlap");
    {
        const size_t size = 2ul << 20;
        const int n = 32;
        void *p[n];
        for (int i = 0; i < n; i++) {
            p[i] = mmu::map_anon(nullptr, size, 0, mmu::perm_rw);
            CHECK(p[i] != nullptr);
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                uintptr_t a = reinterpret_cast<uintptr_t>(p[i]);
                uintptr_t b = reinterpret_cast<uintptr_t>(p[j]);
                CHECK(a + size <= b || b + size <= a);
            }
        }
        for (int i = 0; i < n; i++) {
            CHECK(!mmu::munmap(p[i], size).bad());
        }
    }

    section("reservations from several threads do not overlap");
    {
        const size_t size = 1ul << 20;
        const int per_thread = 16;
        unsigned threads = n_cpus();
        std::vector<void *> got(threads * per_thread, nullptr);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                got[id * per_thread + i] = mmu::map_anon(nullptr, size, 0, mmu::perm_rw);
            }
        });
        for (void *q : got) {
            CHECK(q != nullptr);
        }
        std::vector<uintptr_t> addr;
        for (void *q : got) {
            if (q) {
                addr.push_back(reinterpret_cast<uintptr_t>(q));
            }
        }
        std::sort(addr.begin(), addr.end());
        for (size_t i = 1; i < addr.size(); i++) {
            CHECK(addr[i - 1] + size <= addr[i]);
        }
        for (void *q : got) {
            if (q) {
                mmu::munmap(q, size);
            }
        }
    }

    section("a reservation made on one thread can be released on another");
    {
        const size_t size = 2ul << 20;
        void *p = nullptr;
        std::thread a([&] { p = mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_rw); });
        a.join();
        bool ok = false;
        std::thread b([&] { ok = !mmu::munmap(p, size).bad(); });
        b.join();
        CHECK(ok);
    }

    section("protect changes permissions without unmapping");
    {
        const size_t size = 2ul << 20;
        void *p = mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_rw);
        CHECK(!mmu::mprotect(p, size, mmu::perm_read).bad());
        CHECK(mmu::isreadable(p, size));
        CHECK(!mmu::mprotect(p, size, mmu::perm_rw).bad());
        memset(p, 1, size);
        CHECK(!mmu::munmap(p, size).bad());
    }
}

void vspace_perf()
{
    group("vspace - performance");

    struct { const char *name; size_t size; int n; } cases[] = {
        {"4 KiB",  4ul << 10, 2000},
        {"2 MiB",  2ul << 20, 1000},
        {"64 MiB", 64ul << 20, 64},
    };

    section("reserve + release");
    for (auto &c : cases) {
        auto t0 = clk::now();
        for (int i = 0; i < c.n; i++) {
            void *p = mmu::map_anon(nullptr, c.size, 0, mmu::perm_rw);
            mmu::munmap(p, c.size);
        }
        char label[64];
        snprintf(label, sizeof(label), "map_anon + munmap %s, no backing", c.name);
        report_ns(label, since(t0), c.n);
    }

    section("reserve + release, scaling");
    {
        const size_t size = 2ul << 20;
        const int total = 2000;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    void *p = mmu::map_anon(nullptr, size, 0, mmu::perm_rw);
                    escape(p);
                    mmu::munmap(p, size);
                }
            });
            report_scale("map_anon + munmap 2 MiB", t, static_cast<double>(n) * t, s);
        }
    }

    section("lookup");
    {
        const size_t size = 2ul << 20;
        for (int live : {1, 100, 1000}) {
            std::vector<void *> p;
            for (int i = 0; i < live; i++) {
                p.push_back(mmu::map_anon(nullptr, size, 0, mmu::perm_rw));
            }
            const int probes = 20000;
            char *target = static_cast<char *>(p[p.size() / 2]);
            auto t0 = clk::now();
            for (int i = 0; i < probes; i++) {
                mmu::ismapped(target, mmu::page_size);
            }
            char label[64];
            snprintf(label, sizeof(label), "ismapped with %d live reservations", live);
            report_ns(label, since(t0), probes);
            for (void *q : p) {
                mmu::munmap(q, size);
            }
        }
    }
}

/* mapping ----------------------------------------------------------------- */

void mapping_functional()
{
    group("mapping");

    section("anonymous memory faults in zeroed and keeps what is written");
    {
        const size_t size = 4ul << 20;
        char *p = static_cast<char *>(mmu::map_anon(nullptr, size, 0, mmu::perm_rw));
        CHECK(p != nullptr);
        bool zero = true;
        for (size_t off = 0; off < size; off += mmu::page_size) {
            zero = zero && p[off] == 0;
        }
        CHECK(zero);
        for (size_t off = 0; off < size; off += mmu::page_size) {
            p[off] = static_cast<char>(off / mmu::page_size);
        }
        bool kept = true;
        for (size_t off = 0; off < size; off += mmu::page_size) {
            kept = kept && p[off] == static_cast<char>(off / mmu::page_size);
        }
        CHECK(kept);
        CHECK(!mmu::munmap(p, size).bad());
    }

    section("concurrent faults on one region");
    {
        const size_t size = 16ul << 20;
        char *p = static_cast<char *>(mmu::map_anon(nullptr, size, 0, mmu::perm_rw));
        unsigned threads = n_cpus();
        parallel(threads, [&](unsigned id) {
            size_t stride = mmu::page_size * threads;
            for (size_t off = id * mmu::page_size; off < size; off += stride) {
                p[off] = 42;
            }
        });
        bool ok = true;
        for (size_t off = 0; off < size; off += mmu::page_size) {
            ok = ok && p[off] == 42;
        }
        CHECK(ok);
        CHECK(!mmu::munmap(p, size).bad());
    }

    // Note: pages of an mmap'd region cannot be checked for distinct backing
    // frames today. mmu::virt_to_phys asserts the address is linear-mapped, and
    // the page-table walk that would answer it (virt_to_phys_pt) is private to
    // core/mmu.cc. mapping::walk makes this testable.

    section("pages within a region are independent");
    {
        const size_t size = 256ul << 10;
        unsigned char *p = static_cast<unsigned char *>(
            mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_rw));
        CHECK(p != nullptr);
        size_t pages = size / mmu::page_size;
        for (size_t i = 0; i < pages; i++) {
            memset(p + i * mmu::page_size, static_cast<int>(i & 0xff), mmu::page_size);
        }
        bool ok = true;
        for (size_t i = 0; i < pages; i++) {
            ok = ok && p[i * mmu::page_size] == (i & 0xff);
            ok = ok && p[i * mmu::page_size + mmu::page_size - 1] == (i & 0xff);
        }
        CHECK(ok);
        CHECK(!mmu::munmap(p, size).bad());
    }
}

void mapping_perf()
{
    group("mapping - performance");

    section("first touch");
    {
        const size_t size = 64ul << 20;
        char *p = static_cast<char *>(mmu::map_anon(nullptr, size, 0, mmu::perm_rw));
        auto t0 = clk::now();
        for (size_t off = 0; off < size; off += mmu::page_size) {
            p[off] = 1;
        }
        report_ns("fault + populate, 4 KiB", since(t0), size / mmu::page_size);
        mmu::munmap(p, size);
    }

    section("populate at map time");
    {
        const size_t size = 16ul << 20;
        const int n = 16;
        auto t0 = clk::now();
        for (int i = 0; i < n; i++) {
            void *p = mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_rw);
            mmu::munmap(p, size);
        }
        report_ns("mmap_populate + munmap, per 4 KiB page",
                  since(t0), static_cast<double>(n) * size / mmu::page_size);
    }

    section("map + touch + unmap, scaling");
    {
        const size_t size = 4ul << 20;
        const int total = 128;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t, 2);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    char *p = static_cast<char *>(mmu::map_anon(nullptr, size, 0, mmu::perm_rw));
                    for (size_t off = 0; off < size; off += mmu::page_size) {
                        p[off] = 1;
                    }
                    mmu::munmap(p, size);
                }
            });
            report_scale("map+touch+unmap 4 MiB, per page", t,
                         static_cast<double>(n) * t * size / mmu::page_size, s);
        }
    }

    section("protect");
    {
        const size_t size = 2ul << 20;
        const int n = 200;
        void *p = mmu::map_anon(nullptr, size, mmu::mmap_populate, mmu::perm_rw);
        auto t0 = clk::now();
        for (int i = 0; i < n; i++) {
            mmu::mprotect(p, size, mmu::perm_read);
            mmu::mprotect(p, size, mmu::perm_rw);
        }
        report_ns("mprotect 2 MiB", since(t0), 2.0 * n);
        mmu::munmap(p, size);
    }
}

/* heap -------------------------------------------------------------------- */

void heap_functional()
{
    group("heap");

    section("allocations are distinct, aligned and usable");
    {
        const size_t sizes[] = {1, 8, 17, 64, 100, 512, 4096, 40000, 300000, 4ul << 20};
        std::vector<void *> p;
        for (size_t s : sizes) {
            void *q = malloc(s);
            CHECK(q != nullptr);
            memset(q, 0x33, s);
            p.push_back(q);
        }
        for (size_t i = 0; i < p.size(); i++) {
            for (size_t j = i + 1; j < p.size(); j++) {
                CHECK(p[i] != p[j]);
            }
        }
        for (void *q : p) {
            free(q);
        }
    }

    section("realloc preserves contents, calloc zeroes");
    {
        char *q = static_cast<char *>(malloc(64));
        memset(q, 0x77, 64);
        q = static_cast<char *>(realloc(q, 8192));
        CHECK(q != nullptr);
        bool ok = true;
        for (int i = 0; i < 64; i++) {
            ok = ok && q[i] == 0x77;
        }
        CHECK(ok);
        free(q);

        char *z = static_cast<char *>(calloc(1024, 4));
        bool zero = true;
        for (int i = 0; i < 4096; i++) {
            zero = zero && z[i] == 0;
        }
        CHECK(zero);
        free(z);
    }

    section("allocate on one thread, free on another");
    {
        const int n = 4096;
        std::vector<void *> p(n);
        std::thread a([&] {
            for (int i = 0; i < n; i++) {
                p[i] = malloc(64 + (i % 512));
            }
        });
        a.join();
        std::atomic<int> freed{0};
        std::thread b([&] {
            for (int i = 0; i < n; i++) {
                free(p[i]);
                freed.fetch_add(1);
            }
        });
        b.join();
        CHECK(freed.load() == n);
    }
}

void heap_perf()
{
    group("heap - performance");

    const int batch = 1024;
    const int rounds = 100;

    section("fixed size, scaling");
    {
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned) {
                std::vector<void *> q(batch);
                for (int r = 0; r < rounds; r++) {
                    for (int i = 0; i < batch; i++) {
                        q[i] = malloc(64);
                        escape(q[i]);
                    }
                    for (int i = 0; i < batch; i++) {
                        free(q[i]);
                    }
                }
            });
            report_scale("malloc(64) + free", t, 2.0 * batch * rounds * t, s);
        }
    }

    section("mixed sizes, scaling");
    {
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned) {
                std::vector<void *> q(batch);
                for (int r = 0; r < rounds; r++) {
                    for (int i = 0; i < batch; i++) {
                        q[i] = malloc(16 + ((i * 37) % 4000));
                        escape(q[i]);
                    }
                    for (int i = 0; i < batch; i++) {
                        free(q[i]);
                    }
                }
            });
            report_scale("malloc mixed + free", t, 2.0 * batch * rounds * t, s);
        }
    }

    section("large");
    {
        const int n = 256;
        for (size_t size : {64ul << 10, 1ul << 20, 16ul << 20}) {
            auto t0 = clk::now();
            for (int i = 0; i < n; i++) {
                void *q = malloc(size);
                escape(q);
                *static_cast<char *>(q) = 1;
                free(q);
            }
            char label[64];
            snprintf(label, sizeof(label), "malloc(%zu KiB) + free + first touch", size >> 10);
            report_ns(label, since(t0), n);
        }
    }

    section("cross-thread free");
    {
        const int n = 20000;
        std::vector<void *> p(n);
        auto t0 = clk::now();
        std::thread a([&] {
            for (int i = 0; i < n; i++) {
                p[i] = malloc(128);
                escape(p[i]);
            }
        });
        a.join();
        std::thread b([&] {
            for (int i = 0; i < n; i++) {
                free(p[i]);
            }
        });
        b.join();
        report_ns("alloc on one thread, free on another", since(t0), 2.0 * n);
    }
}

} // namespace

int os_memory_main()
{
    printf("######## memory subsystem ########\n");
    printf("cpus: %u, memory: %zu MiB\n", n_cpus(), memory::stats::total() >> 20);

    frames_functional();
    frames_perf();
    vspace_functional();
    vspace_perf();
    mapping_functional();
    mapping_perf();
    heap_functional();
    heap_perf();

    int fails = g_fails.load();
    printf("\n%d checks, %d failures\n", g_checks.load(), fails);
    printf("RESULT: %s\n", fails ? "MEMORY TESTS FAILED" : "ALL MEMORY TESTS PASSED");
    return fails ? 1 : 0;
}
