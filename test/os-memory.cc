/*
 * Memory as an application sees it: malloc and mmap.
 *
 * Everything here goes through interfaces that exist on Linux and on OSv as
 * well as on miniOSv, so the numbers can be compared against either and the
 * checks say nothing about how the kernel is built inside. The primitives
 * underneath have their own suite in os-memory-primitives.cc.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <malloc.h>

extern "C" void *reallocarray(void *ptr, size_t nmemb, size_t size);
#include <sys/mman.h>
#include <unistd.h>


#include "mem-test.hh"

using namespace memtest;

namespace {

const size_t page = 4096;
const size_t huge = 2ul << 20;

size_t free_bytes()
{
    long pages = sysconf(_SC_AVPHYS_PAGES);
    return pages > 0 ? static_cast<size_t>(pages) * page : 0;
}

void *map(size_t bytes, int extra = 0)
{
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
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

    section("an allocation is usable to the size it reports");
    {
        // Every size class boundary the allocator might have, and the sizes
        // either side of them. What is checked is the promise malloc makes:
        // the block is at least as big as asked for, malloc_usable_size says
        // how big, and all of that is writable.
        bool ok = true;
        for (size_t n = 1; n <= (4ul << 20); n = n + (n >> 2) + 1) {
            auto *q = static_cast<unsigned char *>(malloc(n));
            CHECK(q != nullptr);
            if (!q) {
                break;
            }
            size_t usable = malloc_usable_size(q);
            ok = ok && usable >= n;
            memset(q, 0xd7, usable);
            ok = ok && q[usable - 1] == 0xd7;
            // Aligned for any type, which is what malloc promises and what a
            // size class rounded to a power of two gives for free.
            ok = ok && (reinterpret_cast<uintptr_t>(q) & (alignof(max_align_t) - 1)) == 0;
            free(q);
        }
        CHECK(ok);
    }

    section("an aligned allocation is aligned");
    {
        bool ok = true;
        for (size_t align = sizeof(void *); align <= 2048; align *= 2) {
            for (size_t n : {align / 2, align, align * 3, size_t(100000)}) {
                void *q = nullptr;
                ok = ok && posix_memalign(&q, align, n ? n : 1) == 0;
                ok = ok && q != nullptr;
                if (!q) {
                    continue;
                }
                ok = ok && (reinterpret_cast<uintptr_t>(q) & (align - 1)) == 0;
                free(q);
            }
        }
        CHECK(ok);
    }

    section("the odd corners of the malloc family hold");
    {
        void *z = malloc(0);
        free(z);

        void *p = realloc(nullptr, 100);
        CHECK(p != nullptr);
        memset(p, 1, 100);
        free(realloc(p, 0));

        CHECK(reallocarray(nullptr, SIZE_MAX / 2, 4) == nullptr);
        // Through a volatile pointer, or the compiler folds the builtin's
        // null-check away on the assumption that allocation succeeds.
        void *(*volatile vcalloc)(size_t, size_t) = calloc;
        CHECK(vcalloc(SIZE_MAX / 2, 4) == nullptr);
        CHECK(malloc_usable_size(nullptr) == 0);

        void *a = aligned_alloc(256, 512);
        CHECK(a != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(a) & 255) == 0);
        free(a);

        // memalign does not require the size to be a multiple.
        void *m = memalign(512, 100);
        CHECK(m != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(m) & 511) == 0);
        free(m);
    }

    section("delete gives back what new took, sized or not");
    {
        // The sized form of operator delete is what the compiler emits for a
        // type it knows, and it is a different entry point from free(). Both
        // have to reach the same allocator, or one of them corrupts it.
        struct block { char c[200]; };
        const int n = 20000;
        std::vector<block *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = new block;          // NOLINT: the point is the delete below
            p[i]->c[0] = char(i);
        }
        bool ok = true;
        for (int i = 0; i < n; i++) {
            ok = ok && p[i]->c[0] == char(i);
        }
        CHECK(ok);
        for (int i = 0; i < n; i++) {
            delete p[i];               // sized: the type is known here
        }
        // And the unsized form, through the array shape.
        for (int i = 0; i < n; i++) {
            p[i] = reinterpret_cast<block *>(new char[sizeof(block)]);
        }
        for (int i = 0; i < n; i++) {
            delete[] reinterpret_cast<char *>(p[i]);
        }
    }

    section("memory comes back when the objects do");
    {
        // Enough to need many pages of whatever the allocator carves, cycled
        // so that each round can only be served from what the last gave back.
        const int n = 8192;
        const size_t size = 3000;
        std::vector<void *> p(n);
        auto cycle = [&] {
            for (int i = 0; i < n; i++) {
                p[i] = malloc(size);
                escape(p[i]);
            }
            for (int i = 0; i < n; i++) {
                free(p[i]);
            }
        };
        cycle();                       // first round warms whatever is cached
        size_t before = free_bytes();
        for (int r = 0; r < 20; r++) {
            cycle();
        }
        long drift = long(before) - long(free_bytes());
        // 20 rounds of 24 MiB: an allocator that never reused a page would be
        // half a gigabyte down, so the bar is loose and still decisive. It is
        // also free memory of the whole system, which nothing here controls.
        CHECK(drift < long(64ul << 20));
        printf("      %d x %zu B, %d rounds: drift %ld KiB\n", n, size, 20, drift >> 10);
    }

    section("a working set too big to cache is recycled correctly");
    {
        // Big enough that the emptied pages cannot all be held back, so their
        // addresses are handed out again rather than simply reused. That is
        // where an allocator that recycles an address before the hardware has
        // forgotten the old translation goes wrong, and it goes wrong across
        // cpus: the thread that writes sees its own store either way, and only
        // another one reading the same address sees the wrong frame.
        const size_t size = 64ul << 10;
        size_t budget = free_bytes() / 8 / n_cpus() / size;
        size_t each = std::min(std::max(budget, size_t(16)), size_t(2048));

        std::atomic<int> bad{0};
        size_t before = free_bytes();
        for (int round = 0; round < 8; round++) {
            parallel(n_cpus(), [&](unsigned id) {
                std::vector<char *> q(each);
                for (size_t i = 0; i < each; i++) {
                    q[i] = static_cast<char *>(malloc(size));
                    if (!q[i]) {
                        bad.fetch_add(1);
                        continue;
                    }
                    // One mark per 4 KiB, so a page recycled underneath the
                    // block shows up wherever the seam falls.
                    for (size_t off = 0; off < size; off += page) {
                        q[i][off] = char(id * 31 + i);
                    }
                }
                for (size_t i = 0; i < each; i++) {
                    if (!q[i]) {
                        continue;
                    }
                    for (size_t off = 0; off < size; off += page) {
                        if (q[i][off] != char(id * 31 + i)) {
                            bad.fetch_add(1);
                        }
                    }
                    free(q[i]);
                }
            });
        }
        CHECK(bad.load() == 0);
        // What is still out is whatever the allocator caches -- a fraction of
        // memory, by design. The bar is a quarter of it, which a leak of even
        // one round's working set would clear.
        long drift = long(before) - long(free_bytes());
        CHECK(drift < long(before / 4));
        printf("      %zu x %zu KiB per cpu, 8 rounds: drift %ld KiB of %ld MiB\n",
               each, size >> 10, drift >> 10, long(before >> 20));
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

/* mmap -------------------------------------------------------------------- */

void mmap_functional()
{
    group("mmap");

    section("a mapping is usable and unmaps cleanly");
    {
        const size_t size = 2ul << 20;
        char *p = static_cast<char *>(map(size));
        CHECK(p != nullptr);
        memset(p, 0x5a, size);
        CHECK(p[0] == 0x5a);
        CHECK(p[size - 1] == 0x5a);
        CHECK(munmap(p, size) == 0);
    }

    section("mappings do not overlap");
    {
        const size_t size = 2ul << 20;
        const int n = 32;
        void *p[n];
        for (int i = 0; i < n; i++) {
            p[i] = map(size);
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
            CHECK(munmap(p[i], size) == 0);
        }
    }

    section("mappings from several threads do not overlap");
    {
        const size_t size = 1ul << 20;
        const int per_thread = 16;
        unsigned threads = n_cpus();
        std::vector<void *> got(threads * per_thread, nullptr);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                got[id * per_thread + i] = map(size);
            }
        });
        std::vector<uintptr_t> addr;
        for (void *q : got) {
            CHECK(q != nullptr);
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
                munmap(q, size);
            }
        }
    }

    section("a mapping made on one thread can be unmapped on another");
    {
        const size_t size = 2ul << 20;
        void *p = nullptr;
        std::thread a([&] { p = map(size, MAP_POPULATE); });
        a.join();
        int rc = -1;
        std::thread b([&] { rc = munmap(p, size); });
        b.join();
        CHECK(rc == 0);
    }

    section("a mapping that needs a file is refused");
    {
        // There is no filesystem behind mmap here, and even where there is
        // one, fd -1 without MAP_ANONYMOUS can never be served.
        void *p = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0);
        CHECK(p == MAP_FAILED);
    }

    section("mapping spends the memory at once, and gives it back");
    {
        const size_t size = 64ul << 20;
        size_t before = free_bytes();
        char *p = static_cast<char *>(map(size));
        CHECK(p != nullptr);
        CHECK(before - free_bytes() >= size);
        for (size_t off = 0; off < size; off += page) {
            p[off] = 1;
        }
        CHECK(munmap(p, size) == 0);
        CHECK(free_bytes() + (size / 4) >= before);
    }

    section("MAP_POPULATE spends the memory up front");
    {
        const size_t size = 8ul << 20;
        size_t before = free_bytes();
        void *p = map(size, MAP_POPULATE);
        CHECK(p != nullptr);
        CHECK(before - free_bytes() >= size / 2);
        CHECK(munmap(p, size) == 0);
    }

    section("mprotect changes access without unmapping");
    {
        const size_t size = 2ul << 20;
        char *p = static_cast<char *>(map(size, MAP_POPULATE));
        memset(p, 7, size);
        CHECK(mprotect(p, size, PROT_READ) == 0);
        CHECK(p[0] == 7);
        CHECK(p[size - 1] == 7);
        CHECK(mprotect(p, size, PROT_READ | PROT_WRITE) == 0);
        memset(p, 8, size);
        CHECK(p[size - 1] == 8);
        CHECK(munmap(p, size) == 0);
    }

    // The frames under a mapping belong to it until it is unmapped, so this
    // is accepted and keeps them.
    section("MADV_DONTNEED is accepted and changes nothing");
    {
        const size_t size = 8ul << 20;
        char *p = static_cast<char *>(map(size, MAP_POPULATE));
        memset(p, 9, size);
        size_t populated = free_bytes();
        CHECK(madvise(p, size, MADV_DONTNEED) == 0);
        CHECK(free_bytes() == populated);
        CHECK(p[0] == 9);
        CHECK(p[size - 1] == 9);
        CHECK(munmap(p, size) == 0);
    }

    section("msync on anonymous memory succeeds, and fails off the map");
    {
        const size_t size = 2ul << 20;
        char *p = static_cast<char *>(map(size));
        CHECK(msync(p, size, MS_SYNC) == 0);
        CHECK(munmap(p, size) == 0);
        CHECK(msync(p, size, MS_SYNC) != 0);
    }

    section("these calls answer only for what mmap handed out");
    {
        // A page-aligned malloc, so the refusal is about ownership and not
        // about the alignment every one of these checks first.
        void *m = aligned_alloc(page, page);
        CHECK(m != nullptr);
        CHECK(munmap(m, page) != 0);
        CHECK(mprotect(m, page, PROT_READ) != 0);
        CHECK(madvise(m, page, MADV_DONTNEED) != 0);
        CHECK(msync(m, page, MS_SYNC) != 0);
        free(m);
    }
}

void mmap_perf()
{
    group("mmap - performance");

    struct { const char *name; size_t size; int n; } cases[] = {
        {"4 KiB",  4ul << 10, 2000},
        {"2 MiB",  2ul << 20, 1000},
        {"64 MiB", 64ul << 20, 64},
    };

    section("map + unmap, nothing touched");
    for (auto &c : cases) {
        auto t0 = clk::now();
        for (int i = 0; i < c.n; i++) {
            void *p = map(c.size);
            escape(p);
            munmap(p, c.size);
        }
        char label[64];
        snprintf(label, sizeof(label), "mmap + munmap %s", c.name);
        report_ns(label, since(t0), c.n);
    }

    section("map + unmap, scaling");
    {
        const size_t size = 2ul << 20;
        const int total = 2000;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    void *p = map(size);
                    escape(p);
                    munmap(p, size);
                }
            });
            report_scale("mmap + munmap 2 MiB", t, static_cast<double>(n) * t, s);
        }
    }

    section("mprotect");
    {
        const size_t size = 2ul << 20;
        const int n = 200;
        void *p = map(size, MAP_POPULATE);
        auto t0 = clk::now();
        for (int i = 0; i < n; i++) {
            mprotect(p, size, PROT_READ);
            mprotect(p, size, PROT_READ | PROT_WRITE);
        }
        report_ns("mprotect 2 MiB", since(t0), 2.0 * n);
        munmap(p, size);
    }

}

/* page faults ------------------------------------------------------------- */

void fault_functional()
{
    group("mapped memory");

    section("a fresh mapping is zeroed and keeps what is written");
    {
        const size_t size = 4ul << 20;
        char *p = static_cast<char *>(map(size));
        CHECK(p != nullptr);
        bool zero = true;
        for (size_t off = 0; off < size; off += page) {
            zero = zero && p[off] == 0;
        }
        CHECK(zero);
        for (size_t off = 0; off < size; off += page) {
            p[off] = static_cast<char>(off / page);
        }
        bool kept = true;
        for (size_t off = 0; off < size; off += page) {
            kept = kept && p[off] == static_cast<char>(off / page);
        }
        CHECK(kept);
        CHECK(munmap(p, size) == 0);
    }

    section("threads touching one mapping at once each get their own pages");
    {
        const size_t size = 16ul << 20;
        char *p = static_cast<char *>(map(size));
        unsigned threads = n_cpus();
        parallel(threads, [&](unsigned id) {
            size_t stride = page * threads;
            for (size_t off = id * page; off < size; off += stride) {
                p[off] = static_cast<char>(id + 1);
            }
        });
        bool ok = true;
        for (size_t off = 0; off < size; off += page) {
            unsigned who = (off / page) % threads;
            ok = ok && p[off] == static_cast<char>(who + 1);
        }
        CHECK(ok);
        CHECK(munmap(p, size) == 0);
    }

    section("writing one page leaves its neighbours alone");
    {
        const size_t size = 256ul << 10;
        unsigned char *p = static_cast<unsigned char *>(map(size, MAP_POPULATE));
        CHECK(p != nullptr);
        size_t pages = size / page;
        for (size_t i = 0; i < pages; i++) {
            memset(p + i * page, static_cast<int>(i & 0xff), page);
        }
        bool ok = true;
        for (size_t i = 0; i < pages; i++) {
            ok = ok && p[i * page] == (i & 0xff);
            ok = ok && p[i * page + page - 1] == (i & 0xff);
        }
        CHECK(ok);
        CHECK(munmap(p, size) == 0);
    }
}

void fault_perf()
{
    group("page faults - performance");

    section("first touch");
    {
        const size_t size = 64ul << 20;
        char *p = static_cast<char *>(map(size));
        auto t0 = clk::now();
        for (size_t off = 0; off < size; off += page) {
            p[off] = 1;
        }
        report_ns("fault + populate, 4 KiB", since(t0), size / page);
        munmap(p, size);
    }

    section("populate at map time instead");
    {
        const size_t size = 16ul << 20;
        const int n = 16;
        auto t0 = clk::now();
        for (int i = 0; i < n; i++) {
            void *p = map(size, MAP_POPULATE);
            munmap(p, size);
        }
        report_ns("MAP_POPULATE + munmap, per 4 KiB page",
                  since(t0), static_cast<double>(n) * size / page);
    }

    section("map + touch + unmap, scaling");
    {
        const size_t size = 4ul << 20;
        const int total = 128;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t, 2);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    char *p = static_cast<char *>(map(size));
                    for (size_t off = 0; off < size; off += page) {
                        p[off] = 1;
                    }
                    munmap(p, size);
                }
            });
            report_scale("map+touch+unmap 4 MiB, per page", t,
                         static_cast<double>(n) * t * size / page, s);
        }
    }

    section("one mapping faulted by every thread, scaling");
    {
        // Constant total work, so throughput rising with threads means the
        // fault path really is parallel. Every thread faults pages of the same
        // mapping, which is what a database with one large mapping does.
        const size_t size = 256ul << 20;
        const double pages = static_cast<double>(size / page);
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            char *p = static_cast<char *>(map(size));
            if (!p) {
                break;
            }
            double s = parallel(t, [&](unsigned id) {
                size_t stride = page * t;
                for (size_t off = id * page; off < size; off += stride) {
                    p[off] = 1;
                }
            });
            report_scale("one shared mapping, per page", t, pages, s);
            munmap(p, size);
        }
    }

    section("first touch of a huge-page-aligned mapping");
    {
        const size_t size = 64ul << 20;
        char *p = static_cast<char *>(map(size));
        auto t0 = clk::now();
        for (size_t off = 0; off < size; off += huge) {
            p[off] = 1;
        }
        report_ns("fault + populate, one touch per 2 MiB", since(t0), size / huge);
        munmap(p, size);
    }
}

}

int os_memory_main()
{
    reset();
    printf("######## memory, as an application sees it ########\n");
    printf("cpus: %u, free: %zu MiB\n", n_cpus(), free_bytes() >> 20);

    heap_functional();
    heap_perf();
    mmap_functional();
    mmap_perf();
    fault_functional();
    fault_perf();

    return summary("MEMORY");
}
