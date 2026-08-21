/*
 * Default eviction policy inspired by S3-FIFO[SOSP'23]
 * It differs slightly by using the accessed bit instead of tracking accesses explicitly
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <atomic>
#include <cstdlib>

#include <osv/debug.hh>
#include <osv/ilog2.hh>
#include <osv/preempt-lock.hh>
#include <osv/sched.hh>

#include "internal.hh"

namespace mem {
namespace pagecache {

namespace {

// The small queue's share: one tenth on probation, the rest proven.
constexpr size_t small_share = 10;

// Buffers one call to evict() looks at before giving up on finding a cold one.
constexpr unsigned steps_max = 4096;

constexpr int64_t ring_cap0 = 1024;
constexpr unsigned retired_max = 40;

struct ring {
    int64_t cap;                // a power of two
    std::atomic<buffer *> *slot;
};

ring *ring_make(int64_t cap)
{
    auto *r = static_cast<ring *>(
        std::malloc(sizeof(ring) + cap * sizeof(std::atomic<buffer *>)));
    if (r) {
        r->cap = cap;
        r->slot = reinterpret_cast<std::atomic<buffer *> *>(r + 1);
    }
    return r;
}

std::atomic<buffer *> &at(ring *r, int64_t i)
{
    return r->slot[i & (r->cap - 1)];
}

// Single producer at the bottom (the cpu owning it, preemption off), any
// number of stealers at the top. Indices only grow, so there is no aba.
struct deque {
    std::atomic<int64_t> top{0}, bottom{0};
    std::atomic<ring *> arr{nullptr};
    std::atomic<size_t> bytes{0};
    ring *retired[retired_max];
    unsigned nretired = 0;
};

struct shard {
    deque small;
    deque main;
};

struct queues {
    shard *at;
    unsigned n;
    std::atomic<unsigned> turn{0};

    // The names of what the small queues gave up: one slot per hash, the
    // newest name winning it. Forgets early, invents nothing.
    std::atomic<uint32_t> *ghost;
    size_t ghost_mask;
};

buffer *steal(deque &q)
{
    int64_t t = q.top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t bot = q.bottom.load(std::memory_order_acquire);
    if (t >= bot) {
        return nullptr;
    }
    ring *a = q.arr.load(std::memory_order_acquire);
    buffer *b = at(a, t).load(std::memory_order_relaxed);
    if (!q.top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst)) {
        return nullptr;
    }
    q.bytes.fetch_sub(size(*b), std::memory_order_relaxed);
    return b;
}

// Push onto the deque of whichever cpu this runs on. The ring is grown, and a
// leftover spare freed, with preemption enabled.
void enqueue(queues &s, deque shard::*which, buffer *b)
{
    ring *spare = nullptr;
    for (;;) {
        int64_t need = 0;
        bool done = false;
        {
            SCOPE_LOCK(preempt_lock);
            deque &q = s.at[sched::cpu::current()->id % s.n].*which;
            int64_t bot = q.bottom.load(std::memory_order_relaxed);
            int64_t top = q.top.load(std::memory_order_acquire);
            ring *a = q.arr.load(std::memory_order_relaxed);
            if (spare && spare->cap > a->cap) {
                for (int64_t i = top; i < bot; i++) {
                    at(spare, i).store(at(a, i).load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
                }
                if (q.nretired == retired_max) {
                    abort("pagecache: policy queue grew out of bounds\n");
                }
                q.retired[q.nretired++] = a;
                q.arr.store(spare, std::memory_order_release);
                a = spare;
                spare = nullptr;
            }
            if (bot - top < a->cap) {
                at(a, bot).store(b, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                q.bottom.store(bot + 1, std::memory_order_relaxed);
                q.bytes.fetch_add(size(*b), std::memory_order_relaxed);
                done = true;
            } else {
                need = a->cap * 2;
            }
        }
        if (done) {
            std::free(spare);
            return;
        }
        std::free(spare);
        spare = ring_make(need);
        if (!spare) {
            abort("pagecache: no memory for the policy queue\n");
        }
    }
}

void deque_destroy(deque &q)
{
    for (unsigned i = 0; i < q.nretired; i++) {
        std::free(q.retired[i]);
    }
    std::free(q.arr.load(std::memory_order_relaxed));
}

uint32_t name_of(uint64_t off)
{
    uint64_t h = (off / page_size) * 0x9e3779b97f4a7c15ull;
    return uint32_t(h >> 32) | 1;
}

bool remembered(queues &s, uint64_t off)
{
    uint32_t n = name_of(off);
    return s.ghost[n & s.ghost_mask].load(std::memory_order_relaxed) == n;
}

void remember(queues &s, uint64_t off)
{
    uint32_t n = name_of(off);
    s.ghost[n & s.ghost_mask].store(n, std::memory_order_relaxed);
}

void *create(uint64_t store_size)
{
    auto *s = new (std::nothrow) queues();
    if (!s) {
        return nullptr;
    }
    s->n = sched::cpus.size();
    s->at = static_cast<shard *>(std::calloc(s->n, sizeof(shard)));

    uint64_t want = std::min<uint64_t>(
        std::max<uint64_t>(store_size / page_size / 8, 1024), 1u << 20);
    s->ghost_mask = (size_t(1) << ilog2_roundup(want)) - 1;
    s->ghost = static_cast<std::atomic<uint32_t> *>(
        std::calloc(s->ghost_mask + 1, sizeof(uint32_t)));

    bool ok = s->at && s->ghost;
    for (unsigned i = 0; ok && i < s->n; i++) {
        new (&s->at[i]) shard();
        s->at[i].small.arr.store(ring_make(ring_cap0), std::memory_order_relaxed);
        s->at[i].main.arr.store(ring_make(ring_cap0), std::memory_order_relaxed);
        ok = s->at[i].small.arr.load(std::memory_order_relaxed) &&
             s->at[i].main.arr.load(std::memory_order_relaxed);
    }
    if (!ok) {
        std::free(s->at);
        std::free(s->ghost);
        delete s;
        return nullptr;
    }
    return s;
}

void destroy(void *p)
{
    auto *s = static_cast<queues *>(p);
    if (!s) {
        return;
    }
    for (unsigned i = 0; i < s->n; i++) {
        deque_destroy(s->at[i].small);
        deque_destroy(s->at[i].main);
    }
    std::free(s->at);
    std::free(s->ghost);
    delete s;
}

size_t fault_size(void *, uint64_t)
{
    return page_size;
}

void on_fault(void *p, buffer &b)
{
    auto *s = static_cast<queues *>(p);
    if (!s) {
        return;
    }
    bool proven = remembered(*s, offset(b));
    enqueue(*s, proven ? &shard::main : &shard::small, &b);
}

void evict(void *p, size_t bytes, buffer_list &victims)
{
    auto *s = static_cast<queues *>(p);
    if (!s) {
        return;
    }
    size_t got = 0;
    unsigned dry = 0;
    for (unsigned step = 0;
         got < bytes && !victims.full() && dry < 2 * s->n && step < steps_max; step++) {
        shard &sh = s->at[s->turn.fetch_add(1, std::memory_order_relaxed) % s->n];
        size_t sb = sh.small.bytes.load(std::memory_order_relaxed);
        size_t mb = sh.main.bytes.load(std::memory_order_relaxed);
        // Probation is over once the small queue exceeds its share.
        bool from_small = sb * small_share > sb + mb;
        buffer *b = steal(from_small ? sh.small : sh.main);
        if (!b) {
            from_small = !from_small;
            b = steal(from_small ? sh.small : sh.main);
        }
        if (!b) {
            dry++;
            continue;
        }
        dry = 0;
        if (accessed(*b)) {
            // The second chance: the clear costs the same invalidation the
            // eviction it avoids would have.
            clear_accessed(*b);
            enqueue(*s, &shard::main, b);
            continue;
        }
        if (from_small) {
            remember(*s, offset(*b));
        }
        victims.add(b);
        got += size(*b);
    }
}

void on_evicted(void *, buffer &)
{
}

const policy s3fifo = {
    .bytes_per_buffer = 0,
    .create = create,
    .destroy = destroy,
    .fault_size = fault_size,
    .prefetch = nullptr,
    .evict = evict,
    .on_fault = on_fault,
    .on_evicted = on_evicted,
    // Null: the cache reads the dirty bits after the shootdown, which is the
    // accurate source.
    .is_dirty = nullptr,
};

} // namespace

const policy &defaults()
{
    return s3fifo;
}

}
}
