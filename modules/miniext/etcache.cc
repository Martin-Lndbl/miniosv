/*
 * A cache of extent tree blocks.
 *
 * A lookup that cannot be answered from the inode descends into a tree block
 * on the device, so without a cache every read of a fragmented file costs two
 * device round trips instead of one: the tree block, then the data. The tree
 * blocks are few -- four for a 40 GiB file -- and every lookup on that file
 * goes through them, so a very small cache removes the extra trip entirely.
 *
 * How much it holds is configurable, down to nothing, so that what it costs in
 * memory can be weighed against what it saves.
 *
 * The buffers are allocated once, when the cache is configured, and reused:
 * both a hit and a miss are then free of allocation, which matters because the
 * alternative is a frame allocation and a page mapping on the read path.
 */

#include <cstring>

#include "internal.hh"

namespace miniext {

// How many blocks the next mount's cache will hold. A default that is small
// enough not to matter and large enough to cover the tree of a big file: four
// blocks map 40 GiB, so this is room for a deep tree or several open files.
namespace {
unsigned g_extent_cache_blocks = 64;
}

void extent_cache_configure(unsigned blocks) { g_extent_cache_blocks = blocks; }
unsigned extent_cache_configured() { return g_extent_cache_blocks; }

namespace etcache {

namespace {

struct entry {
    uint64_t block = 0;         // 0 is never an extent tree block, so it means empty
    uint8_t *data = nullptr;
    mem::frames::phys_addr pa = mem::frames::no_memory;
    uint64_t used = 0;          // for choosing a victim; higher is more recent
};

mutex lock;
std::vector<entry> entries;
uint32_t entry_size;
uint64_t clock;
stats counters;

// Caller holds the lock.
entry *find(uint64_t block)
{
    for (auto &e : entries) {
        if (e.block == block) {
            return &e;
        }
    }
    return nullptr;
}

// Caller holds the lock. The least recently used, or the first empty one.
entry *victim()
{
    entry *worst = nullptr;
    for (auto &e : entries) {
        if (e.block == 0) {
            return &e;
        }
        if (!worst || e.used < worst->used) {
            worst = &e;
        }
    }
    return worst;
}

void release_all()
{
    for (auto &e : entries) {
        if (e.pa != mem::frames::no_memory) {
            mem::frames::free(e.pa, entry_size);
        }
    }
    entries.clear();
}

} // namespace

void configure(unsigned blocks, uint32_t block_size)
{
    WITH_LOCK(lock) {
        release_all();
        entry_size = block_size;
        counters = stats{};

        for (unsigned i = 0; i < blocks; i++) {
            entry e;
            e.pa = mem::frames::alloc(block_size, block_size);
            if (e.pa == mem::frames::no_memory) {
                break;          // whatever was allocated is still usable
            }
            e.data = static_cast<uint8_t *>(mem::map_phys(e.pa, block_size));
            entries.push_back(e);
        }
    }
}

void teardown()
{
    WITH_LOCK(lock) {
        release_all();
        counters = stats{};
    }
}

unsigned capacity()
{
    WITH_LOCK(lock) {
        return entries.size();
    }
}

int read(fs *f, uint64_t block, uint8_t *dst)
{
    WITH_LOCK(lock) {
        entry *e = find(block);
        if (e) {
            memcpy(dst, e->data, entry_size);
            e->used = ++clock;
            counters.hits++;
            return 0;
        }
    }

    // A miss reads without the lock held: a device read is far too long to
    // hold every other reader out for, and the worst a concurrent miss on the
    // same block can do is read it twice.
    int rc = f->dev.read(dst, block, 1);
    if (rc < 0) {
        return rc;
    }

    WITH_LOCK(lock) {
        counters.misses++;
        if (entries.empty()) {
            return 0;           // the cache is switched off
        }
        entry *e = find(block);
        if (!e) {
            e = victim();
            if (e->block != 0) {
                counters.evictions++;
            }
            e->block = block;
        }
        memcpy(e->data, dst, entry_size);
        e->used = ++clock;
    }
    return 0;
}

void invalidate(uint64_t block)
{
    WITH_LOCK(lock) {
        entry *e = find(block);
        if (e) {
            e->block = 0;
        }
    }
}

stats report()
{
    WITH_LOCK(lock) {
        stats s = counters;
        s.held = entries.size();
        s.bytes = size_t(entries.size()) * entry_size;
        return s;
    }
}

} // namespace etcache

extent_cache_stats extent_cache_report()
{
    etcache::stats s = etcache::report();
    return extent_cache_stats{s.hits, s.misses, s.evictions, s.held, s.bytes};
}

} // namespace miniext
