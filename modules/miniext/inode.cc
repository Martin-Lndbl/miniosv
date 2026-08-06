/*
 * miniext inodes and the extent tree.
 *
 * Inode `ino` (1-based) lives in group (ino-1)/inodes_per_group at index
 * (ino-1)%inodes_per_group of that group's inode table. Inodes are
 * s_inode_size bytes apart on disk, which is 256 here, but only the first 128
 * bytes are the structure we read.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "internal.hh"

namespace miniext {

uint64_t inode_size(const inode *in)
{
    return (static_cast<uint64_t>(le32(in->i_size_high)) << 32) | le32(in->i_size_lo);
}

bool inode_is_dir(const inode *in)
{
    return (le16(in->i_mode) & S_IFMT_MASK) == MODE_DIR;
}

bool inode_is_reg(const inode *in)
{
    return (le16(in->i_mode) & S_IFMT_MASK) == MODE_REG;
}

int inode_read(fs *f, uint32_t ino, inode *out)
{
    if (ino == 0 || ino > le32(f->sb.s_inodes_count)) {
        printf("miniext: inode %u out of range\n", ino);
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

    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int rc = f->dev.read(buf.data(), block, 1);
    if (rc < 0) {
        return rc;
    }

    memcpy(out, buf.data() + in_block, sizeof(inode));
    return 0;
}

// Walk the extent tree rooted in in->i_block to find the physical block backing
// file block `fblock`. Depth 0 means the root holds leaves directly; deeper
// trees hop through index nodes, one block read per level.
//
// A file block with no covering extent is a hole: *phys = 0, and the caller
// reads it as zeros. An uninitialised extent (ee_len > 32768) is also a hole.
int extent_lookup(fs *f, const inode *in, uint32_t fblock,
                  uint64_t *phys, uint32_t *run)
{
    *phys = 0;
    *run = 1;

    if (!(le32(in->i_flags) & INODE_FL_EXTENTS)) {
        printf("miniext: inode has no extent flag; the legacy block map is not "
               "implemented\n");
        return -ENOTSUP;
    }

    scratch node(f->block_size);
    if (!node) {
        return -ENOMEM;
    }

    const uint8_t *cur = in->i_block;
    size_t cur_size = sizeof(in->i_block);

    for (;;) {
        extent_header eh;
        memcpy(&eh, cur, sizeof(eh));

        if (le16(eh.eh_magic) != EXTENT_MAGIC) {
            printf("miniext: bad extent magic 0x%x (expected 0x%x)\n",
                   le16(eh.eh_magic), EXTENT_MAGIC);
            return -EIO;
        }

        const uint16_t entries = le16(eh.eh_entries);
        const uint16_t depth = le16(eh.eh_depth);

        // Guard against a corrupt header claiming more entries than fit.
        const size_t capacity = (cur_size - sizeof(extent_header)) / sizeof(extent);
        if (entries > capacity) {
            printf("miniext: extent header claims %u entries, only %zu fit\n",
                   entries, capacity);
            return -EIO;
        }

        if (depth == 0) {
            const extent *ee = reinterpret_cast<const extent *>(cur + sizeof(extent_header));
            for (uint16_t i = 0; i < entries; i++) {
                const uint32_t start = le32(ee[i].ee_block);
                const uint16_t len = extent_len(&ee[i]);
                if (fblock >= start && fblock < start + len) {
                    if (extent_is_uninit(&ee[i])) {
                        *phys = 0;                      // reads as zeros
                    } else {
                        *phys = extent_start(&ee[i]) + (fblock - start);
                    }
                    *run = start + len - fblock;
                    return 0;
                }
                if (start > fblock) {
                    // Extents are sorted, so a hole runs up to this one.
                    *run = start - fblock;
                    return 0;
                }
            }
            return 0;                                   // hole past the last extent
        }

        // Interior node: descend into the last index whose ei_block <= fblock.
        const extent_idx *ix =
            reinterpret_cast<const extent_idx *>(cur + sizeof(extent_header));
        int chosen = -1;
        for (uint16_t i = 0; i < entries; i++) {
            if (le32(ix[i].ei_block) <= fblock) {
                chosen = i;
            } else {
                break;
            }
        }
        if (chosen < 0) {
            return 0;                                   // before the first index: hole
        }

        const uint64_t child = extent_idx_leaf(&ix[chosen]);
        int rc = f->dev.read(node.data(), child, 1);
        if (rc < 0) {
            return rc;
        }
        cur = node.data();
        cur_size = f->block_size;
    }
}

// Read from an inode's data. Holes yield zeros. Reads are clipped to i_size.
int64_t inode_pread(fs *f, const inode *in, void *buf, size_t len, uint64_t offset)
{
    const uint64_t fsize = inode_size(in);
    if (offset >= fsize) {
        return 0;
    }
    if (len > fsize - offset) {
        len = fsize - offset;
    }
    if (len == 0) {
        return 0;
    }

    uint8_t *out = static_cast<uint8_t *>(buf);
    size_t moved = 0;

    while (moved < len) {
        const uint64_t pos = offset + moved;
        const uint32_t fblock = static_cast<uint32_t>(pos / f->block_size);
        const uint32_t in_block = pos % f->block_size;

        uint64_t phys = 0;
        uint32_t run = 1;
        int rc = extent_lookup(f, in, fblock, &phys, &run);
        if (rc < 0) {
            return rc;
        }

        size_t want = len - moved;
        // Never cross the end of this block on a partial-block access.
        if (in_block != 0 || want < f->block_size) {
            const size_t to_block_end = f->block_size - in_block;
            if (want > to_block_end) {
                want = to_block_end;
            }
        }

        if (phys == 0) {
            // Hole: zeros, without touching the device.
            size_t span = static_cast<size_t>(run) * f->block_size - in_block;
            if (want > span) {
                want = span;
            }
            memset(out + moved, 0, want);
            moved += want;
            continue;
        }

        if (in_block == 0 && want >= f->block_size) {
            // Whole blocks: read straight into the caller's buffer, coalescing
            // as far as the extent run allows. No cache, no bounce.
            uint32_t nblocks = static_cast<uint32_t>(want / f->block_size);
            if (nblocks > run) {
                nblocks = run;
            }
            rc = f->dev.read(out + moved, phys, nblocks);
            if (rc < 0) {
                return rc;
            }
            moved += static_cast<size_t>(nblocks) * f->block_size;
            continue;
        }

        // Partial block: stage through a scratch buffer.
        scratch tmp(f->block_size);
        if (!tmp) {
            return -ENOMEM;
        }
        rc = f->dev.read(tmp.data(), phys, 1);
        if (rc < 0) {
            return rc;
        }
        memcpy(out + moved, tmp.data() + in_block, want);
        moved += want;
    }

    return static_cast<int64_t>(moved);
}

} // namespace miniext
