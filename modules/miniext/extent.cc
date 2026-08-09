/*
 * miniext extent-tree mutation, plus inode writeback.
 *
 * The tree is capped at depth 1: up to 4 entries inline in i_block, and if
 * those are indices, up to 4 leaf blocks of (block_size - 12) / 12 extents
 * each. At 4 KiB blocks that is 4 x 340 = 1360 extents, and one extent covers
 * up to 32768 blocks, so a contiguously allocated file has no trouble reaching
 * many gigabytes. A pathologically fragmented file can exhaust it, and when
 * that happens we fail loudly rather than write a tree we cannot read back.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "internal.hh"

namespace miniext {

namespace {

extent_header *root_header(inode *in)
{
    return reinterpret_cast<extent_header *>(in->i_block);
}
extent *entries_as_extents(extent_header *eh)
{
    return reinterpret_cast<extent *>(reinterpret_cast<uint8_t *>(eh) +
                                      sizeof(extent_header));
}
extent_idx *entries_as_idx(extent_header *eh)
{
    return reinterpret_cast<extent_idx *>(reinterpret_cast<uint8_t *>(eh) +
                                          sizeof(extent_header));
}

uint16_t leaf_capacity(fs *f)
{
    return static_cast<uint16_t>((f->block_size - sizeof(extent_header)) /
                                 sizeof(extent));
}

void set_extent(extent *e, uint32_t fblock, uint64_t phys, uint16_t len)
{
    e->ee_block = le32(fblock);
    e->ee_len = le16(len);
    e->ee_start_lo = le32(static_cast<uint32_t>(phys));
    e->ee_start_hi = le16(static_cast<uint16_t>(phys >> 32));
}

// Insert [fblock, fblock+count) -> [phys, phys+count) into a leaf, keeping the
// entries sorted by logical block.
//
// Sorted order is not cosmetic. extent_lookup() stops as soon as it sees an
// entry starting past the block it wants, and e2fsck rejects a leaf whose
// entries are out of order ("inode N has out of order extents"). An earlier
// version only appended, which held for a sequential writer and broke the
// moment DuckDB wrote a header and then blocks at scattered offsets: the list
// went unsorted, lookups then missed mappings that existed, and the same
// logical block got a second allocation -- inflating i_blocks and leaking the
// first range.
//
// Returns false only when the leaf is full and the entry cannot be merged.
bool leaf_insert(extent_header *eh, uint16_t capacity, uint32_t fblock,
                 uint64_t phys, uint32_t count)
{
    extent *ee = entries_as_extents(eh);
    uint16_t n = le16(eh->eh_entries);

    // First entry that starts after the new one; the insertion point.
    uint16_t at = n;
    for (uint16_t i = 0; i < n; i++) {
        if (le32(ee[i].ee_block) > fblock) {
            at = i;
            break;
        }
    }

    // Grow the preceding extent if this one continues it in both spaces.
    if (at > 0) {
        extent *prev = &ee[at - 1];
        const uint32_t pstart = le32(prev->ee_block);
        const uint16_t plen = extent_len(prev);
        if (!extent_is_uninit(prev) && pstart + plen == fblock &&
            extent_start(prev) + plen == phys &&
            plen + count <= EXTENT_INIT_MAX_LEN) {
            prev->ee_len = le16(static_cast<uint16_t>(plen + count));
            return true;
        }
    }

    // Or extend the following one backwards, which is what an out-of-order
    // writer filling a gap downwards produces.
    if (at < n) {
        extent *next = &ee[at];
        const uint32_t nstart = le32(next->ee_block);
        const uint16_t nlen = extent_len(next);
        if (!extent_is_uninit(next) && fblock + count == nstart &&
            phys + count == extent_start(next) &&
            nlen + count <= EXTENT_INIT_MAX_LEN) {
            set_extent(next, fblock, phys, static_cast<uint16_t>(nlen + count));
            return true;
        }
    }

    if (n >= capacity) {
        return false;
    }
    for (uint16_t i = n; i > at; i--) {
        ee[i] = ee[i - 1];
    }
    set_extent(&ee[at], fblock, phys, static_cast<uint16_t>(count));
    eh->eh_entries = le16(static_cast<uint16_t>(n + 1));
    return true;
}

// Index of the leaf that should hold `fblock`: the last one starting at or
// before it, or the first leaf when `fblock` precedes them all.
uint16_t index_for(extent_header *eh, uint32_t fblock)
{
    extent_idx *ix = entries_as_idx(eh);
    uint16_t n = le16(eh->eh_entries);
    uint16_t chosen = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (le32(ix[i].ei_block) <= fblock) {
            chosen = i;
        } else {
            break;
        }
    }
    return chosen;
}

} // namespace

// --- inode writeback ----------------------------------------------------

int inode_write(fs *f, uint32_t ino, const inode *in)
{
    if (ino == 0 || ino > le32(f->sb.s_inodes_count)) {
        return -EINVAL;
    }
    const uint32_t group = (ino - 1) / f->inodes_per_group;
    const uint32_t index = (ino - 1) % f->inodes_per_group;
    if (group >= f->group_count) {
        return -EINVAL;
    }

    const uint64_t table = le32(f->groups[group].bg_inode_table_lo);
    const uint64_t byte_off = static_cast<uint64_t>(index) * f->inode_size;
    const uint64_t block = table + byte_off / f->block_size;
    const uint32_t in_block = byte_off % f->block_size;

    // Read-modify-write: the block holds other inodes too.
    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int rc = f->dev.read(buf.data(), block, 1);
    if (rc < 0) {
        return rc;
    }
    memcpy(buf.data() + in_block, in, sizeof(inode));
    return f->dev.write(buf.data(), block, 1);
}

// Mark an inode dead. Clearing i_links_count is not enough: e2fsck flags a
// freed inode whose i_dtime is still zero ("Deleted inode N has zero dtime"),
// because that is what distinguishes a deleted inode from a corrupt live one.
void inode_mark_deleted(inode *in)
{
    in->i_links_count = le16(0);
    in->i_dtime = le32(static_cast<uint32_t>(time(nullptr)));
    if (in->i_dtime == 0) {
        in->i_dtime = le32(1);      // any non-zero value will do
    }
}

void inode_set_size(inode *in, uint64_t size)
{
    in->i_size_lo = le32(static_cast<uint32_t>(size));
    in->i_size_high = le32(static_cast<uint32_t>(size >> 32));
}

void inode_add_blocks(fs *f, inode *in, int64_t delta_fs_blocks)
{
    const int64_t per_block = f->block_size / 512;
    int64_t cur = le32(in->i_blocks_lo);
    cur += delta_fs_blocks * per_block;
    if (cur < 0) {
        cur = 0;
    }
    in->i_blocks_lo = le32(static_cast<uint32_t>(cur));
}

void inode_init(fs *f, inode *in, uint16_t mode)
{
    memset(in, 0, sizeof(*in));
    in->i_mode = le16(mode);
    in->i_links_count = le16(1);
    in->i_flags = le32(INODE_FL_EXTENTS);

    // An empty depth-0 tree living in i_block.
    extent_header *eh = root_header(in);
    eh->eh_magic = le16(EXTENT_MAGIC);
    eh->eh_entries = le16(0);
    eh->eh_max = le16(static_cast<uint16_t>(
        (sizeof(in->i_block) - sizeof(extent_header)) / sizeof(extent)));
    eh->eh_depth = le16(0);
    eh->eh_generation = le32(0);
    (void)f;
}

// --- growing the tree ---------------------------------------------------

// Turn a full depth-0 root into a depth-1 root with one leaf block, so there is
// room for more extents. The four inline extents move into the new leaf.
static int grow_to_depth1(fs *f, uint32_t ino, inode *in)
{
    extent_header *eh = root_header(in);
    if (le16(eh->eh_depth) != 0) {
        return 0;
    }

    uint64_t leaf = 0;
    uint32_t got = 0;
    int rc = block_alloc(f, 0, 1, &leaf, &got);
    if (rc < 0) {
        return rc;
    }

    scratch buf(f->block_size);
    if (!buf) {
        block_free(f, leaf, 1);
        return -ENOMEM;
    }
    memset(buf.data(), 0, f->block_size);

    auto *lh = reinterpret_cast<extent_header *>(buf.data());
    lh->eh_magic = le16(EXTENT_MAGIC);
    lh->eh_entries = eh->eh_entries;
    lh->eh_max = le16(leaf_capacity(f));
    lh->eh_depth = le16(0);
    lh->eh_generation = le32(0);
    memcpy(buf.data() + sizeof(extent_header), in->i_block + sizeof(extent_header),
           le16(eh->eh_entries) * sizeof(extent));

    rc = f->dev.write(buf.data(), leaf, 1);
    if (rc < 0) {
        block_free(f, leaf, 1);
        return rc;
    }

    // Re-root: one index entry pointing at the leaf we just wrote.
    const uint32_t first_block =
        le16(eh->eh_entries) ? le32(entries_as_extents(eh)[0].ee_block) : 0;
    memset(in->i_block, 0, sizeof(in->i_block));
    eh = root_header(in);
    eh->eh_magic = le16(EXTENT_MAGIC);
    eh->eh_entries = le16(1);
    eh->eh_max = le16(static_cast<uint16_t>(
        (sizeof(in->i_block) - sizeof(extent_header)) / sizeof(extent_idx)));
    eh->eh_depth = le16(1);
    eh->eh_generation = le32(0);

    extent_idx *ix = entries_as_idx(eh);
    ix[0].ei_block = le32(first_block);
    ix[0].ei_leaf_lo = le32(static_cast<uint32_t>(leaf));
    ix[0].ei_leaf_hi = le16(static_cast<uint16_t>(leaf >> 32));
    ix[0].ei_unused = le16(0);

    inode_add_blocks(f, in, 1);     // the leaf itself counts against i_blocks
    return inode_write(f, ino, in);
}

// Record that [fblock, fblock+count) maps to [phys, phys+count).
static int extent_insert(fs *f, uint32_t ino, inode *in, uint32_t fblock,
                         uint64_t phys, uint32_t count)
{
    extent_header *eh = root_header(in);

    if (le16(eh->eh_depth) == 0) {
        if (leaf_insert(eh, le16(eh->eh_max), fblock, phys, count)) {
            return inode_write(f, ino, in);
        }
        int rc = grow_to_depth1(f, ino, in);
        if (rc < 0) {
            return rc;
        }
        eh = root_header(in);
    }

    // Depth 1. Pick the leaf by logical block rather than always the last one:
    // an out-of-order writer targets whichever leaf covers its range.
    extent_idx *ix = entries_as_idx(eh);
    uint16_t n = le16(eh->eh_entries);
    if (n == 0) {
        return -EIO;
    }

    uint16_t slot = index_for(eh, fblock);
    uint64_t leaf = extent_idx_leaf(&ix[slot]);

    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int rc = f->dev.read(buf.data(), leaf, 1);
    if (rc < 0) {
        return rc;
    }
    auto *lh = reinterpret_cast<extent_header *>(buf.data());
    if (le16(lh->eh_magic) != EXTENT_MAGIC) {
        printf("miniext: bad magic in extent leaf %lu\n", (unsigned long)leaf);
        return -EIO;
    }

    if (leaf_insert(lh, leaf_capacity(f), fblock, phys, count)) {
        rc = f->dev.write(buf.data(), leaf, 1);
        if (rc < 0) {
            return rc;
        }
        // An entry inserted below the leaf's index key makes that key stale,
        // and extent_lookup() descends by comparing against it: a block before
        // the first key looks like a hole and reads as zeros even though it is
        // mapped. Lower the key to the leaf's new first block.
        const uint32_t first = le32(entries_as_extents(lh)[0].ee_block);
        if (first < le32(ix[slot].ei_block)) {
            ix[slot].ei_block = le32(first);
            return inode_write(f, ino, in);
        }
        return 0;
    }

    // The leaf is full: split it in half and retry. Splitting rather than
    // always appending a fresh leaf is what keeps the index sorted when the
    // writes are not.
    if (n >= le16(eh->eh_max)) {
        printf("miniext: extent tree full (%u leaves at depth 1) for inode %u -- "
               "the file is too fragmented for this implementation\n", n, ino);
        return -ENOSPC;
    }

    uint64_t new_leaf = 0;
    uint32_t got = 0;
    rc = block_alloc(f, leaf, 1, &new_leaf, &got);
    if (rc < 0) {
        return rc;
    }

    scratch nbuf(f->block_size);
    if (!nbuf) {
        block_free(f, new_leaf, 1);
        return -ENOMEM;
    }
    memset(nbuf.data(), 0, f->block_size);

    extent *ee = entries_as_extents(lh);
    const uint16_t total = le16(lh->eh_entries);
    const uint16_t keep = static_cast<uint16_t>(total / 2);
    const uint16_t moved = static_cast<uint16_t>(total - keep);

    auto *nh = reinterpret_cast<extent_header *>(nbuf.data());
    nh->eh_magic = le16(EXTENT_MAGIC);
    nh->eh_entries = le16(moved);
    nh->eh_max = le16(leaf_capacity(f));
    nh->eh_depth = le16(0);
    nh->eh_generation = le32(0);
    memcpy(entries_as_extents(nh), &ee[keep], moved * sizeof(extent));

    lh->eh_entries = le16(keep);

    // The new leaf covers the upper half, so its index goes right after the one
    // being split.
    const uint32_t split_block = le32(entries_as_extents(nh)[0].ee_block);
    for (uint16_t i = n; i > slot + 1; i--) {
        ix[i] = ix[i - 1];
    }
    ix[slot + 1].ei_block = le32(split_block);
    ix[slot + 1].ei_leaf_lo = le32(static_cast<uint32_t>(new_leaf));
    ix[slot + 1].ei_leaf_hi = le16(static_cast<uint16_t>(new_leaf >> 32));
    ix[slot + 1].ei_unused = le16(0);
    eh->eh_entries = le16(static_cast<uint16_t>(n + 1));

    // Land the entry in whichever half now owns it.
    extent_header *target = (fblock >= split_block) ? nh : lh;

    if (!leaf_insert(target, leaf_capacity(f), fblock, phys, count)) {
        block_free(f, new_leaf, 1);
        return -ENOSPC;             // both halves full: cannot happen after a split
    }

    rc = f->dev.write(buf.data(), leaf, 1);
    if (rc < 0) {
        return rc;
    }
    rc = f->dev.write(nbuf.data(), new_leaf, 1);
    if (rc < 0) {
        return rc;
    }
    inode_add_blocks(f, in, 1);     // the new leaf counts against i_blocks
    return inode_write(f, ino, in);
}

int extent_map_write(fs *f, uint32_t ino, inode *in, uint32_t fblock,
                     uint32_t want, uint64_t *phys, uint32_t *run)
{
    int rc = extent_lookup(f, in, fblock, phys, run);
    if (rc < 0) {
        return rc;
    }
    if (*phys != 0) {
        return 0;               // already backed
    }

    // Hole. Do not allocate past the end of the hole, or we would shadow a
    // mapping that already exists further on.
    uint32_t need = want;
    if (*run != 0 && need > *run) {
        need = *run;
    }
    if (need == 0) {
        need = 1;
    }

    // Allocate next to whatever backs the preceding block, so sequential
    // writes coalesce into one long extent.
    uint64_t goal = 0;
    if (fblock > 0) {
        uint64_t prev_phys = 0;
        uint32_t prev_run = 0;
        if (extent_lookup(f, in, fblock - 1, &prev_phys, &prev_run) == 0 &&
            prev_phys != 0) {
            goal = prev_phys + 1;
        }
    }

    uint64_t got_phys = 0;
    uint32_t got = 0;
    rc = block_alloc(f, goal, need, &got_phys, &got);
    if (rc < 0) {
        return rc;
    }

    rc = extent_insert(f, ino, in, fblock, got_phys, got);
    if (rc < 0) {
        block_free(f, got_phys, got);
        return rc;
    }

    inode_add_blocks(f, in, got);
    *phys = got_phys;
    *run = got;
    return 0;
}

// --- shrinking ----------------------------------------------------------

// Drop every mapping at or after file block `from`, freeing the data blocks and
// any leaf that ends up empty.
int extent_truncate(fs *f, uint32_t ino, inode *in, uint32_t from)
{
    extent_header *eh = root_header(in);

    auto trim_leaf = [&](extent_header *lh, bool *changed) -> int {
        extent *ee = entries_as_extents(lh);
        uint16_t n = le16(lh->eh_entries);
        uint16_t keep = n;

        for (uint16_t i = 0; i < n; i++) {
            const uint32_t start = le32(ee[i].ee_block);
            const uint16_t len = extent_len(&ee[i]);

            if (start >= from) {
                if (!extent_is_uninit(&ee[i])) {
                    int rc = block_free(f, extent_start(&ee[i]), len);
                    if (rc < 0) {
                        return rc;
                    }
                    inode_add_blocks(f, in, -static_cast<int64_t>(len));
                }
                if (keep > i) {
                    keep = i;
                }
                *changed = true;
            } else if (start + len > from) {
                const uint16_t new_len = static_cast<uint16_t>(from - start);
                const uint16_t drop = static_cast<uint16_t>(len - new_len);
                if (!extent_is_uninit(&ee[i])) {
                    int rc = block_free(f, extent_start(&ee[i]) + new_len, drop);
                    if (rc < 0) {
                        return rc;
                    }
                    inode_add_blocks(f, in, -static_cast<int64_t>(drop));
                }
                ee[i].ee_len = le16(new_len);
                *changed = true;
            }
        }
        if (keep < n) {
            lh->eh_entries = le16(keep);
        }
        return 0;
    };

    if (le16(eh->eh_depth) == 0) {
        bool changed = false;
        int rc = trim_leaf(eh, &changed);
        if (rc < 0) {
            return rc;
        }
        return inode_write(f, ino, in);
    }

    extent_idx *ix = entries_as_idx(eh);
    uint16_t n = le16(eh->eh_entries);
    uint16_t keep = n;

    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }

    for (uint16_t i = 0; i < n; i++) {
        const uint64_t leaf = extent_idx_leaf(&ix[i]);
        int rc = f->dev.read(buf.data(), leaf, 1);
        if (rc < 0) {
            return rc;
        }
        auto *lh = reinterpret_cast<extent_header *>(buf.data());
        if (le16(lh->eh_magic) != EXTENT_MAGIC) {
            return -EIO;
        }

        bool changed = false;
        rc = trim_leaf(lh, &changed);
        if (rc < 0) {
            return rc;
        }

        if (le16(lh->eh_entries) == 0) {
            // The whole leaf went away; drop it and every index after it.
            rc = block_free(f, leaf, 1);
            if (rc < 0) {
                return rc;
            }
            inode_add_blocks(f, in, -1);
            if (keep > i) {
                keep = i;
            }
        } else if (changed) {
            rc = f->dev.write(buf.data(), leaf, 1);
            if (rc < 0) {
                return rc;
            }
        }
    }

    if (keep < n) {
        eh->eh_entries = le16(keep);
    }
    return inode_write(f, ino, in);
}

} // namespace miniext
