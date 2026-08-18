/*
 * miniext block and inode allocators, and metadata writeback.
 *
 * Bitmap based, one block bitmap and one inode bitmap per group. Everything
 * here runs under fs::alloc_lock: the bitmaps, the group descriptors and the
 * superblock free counters have to move together, and there is no journal to
 * repair a half-finished update.
 *
 * The critical sections are short on purpose -- a writer holds this only while
 * claiming or releasing blocks, never while transferring data.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "internal.hh"

namespace miniext {

namespace {

inline bool bit_test(const uint8_t *bm, uint32_t i)
{
    return (bm[i >> 3] >> (i & 7)) & 1;
}
inline void bit_set(uint8_t *bm, uint32_t i)
{
    bm[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
}
inline void bit_clear(uint8_t *bm, uint32_t i)
{
    bm[i >> 3] &= static_cast<uint8_t>(~(1u << (i & 7)));
}

// Blocks in group g run [first_data_block + g*blocks_per_group, ...), and the
// last group is short whenever the device is not a whole number of groups.
uint32_t blocks_in_group(fs *f, uint32_t g)
{
    uint64_t first = f->first_data_block + static_cast<uint64_t>(g) * f->blocks_per_group;
    uint64_t remaining = f->block_count - first;
    return remaining < f->blocks_per_group ? static_cast<uint32_t>(remaining)
                                           : f->blocks_per_group;
}

} // namespace

// --- metadata writeback -------------------------------------------------

// The superblock is at a fixed byte offset, which for a 4096-byte filesystem
// sits *inside* block 0. Writing it is therefore read-modify-write: clobbering
// the whole block would destroy the boot area ahead of it.
int sb_write(fs *f)
{
    const uint64_t block = SUPERBLOCK_OFFSET / f->block_size;
    const uint32_t off = SUPERBLOCK_OFFSET % f->block_size;

    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int rc = f->dev.read(buf.data(), block, 1);
    if (rc < 0) {
        return rc;
    }
    memcpy(buf.data() + off, &f->sb, sizeof(superblock));
    return f->dev.write(buf.data(), block, 1);
}

int gd_write(fs *f, uint32_t group)
{
    if (group >= f->group_count) {
        return -EINVAL;
    }
    const uint32_t per_block = f->block_size / sizeof(group_desc);
    const uint64_t block = f->first_data_block + 1 + group / per_block;
    const uint32_t off = (group % per_block) * sizeof(group_desc);

    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }
    int rc = f->dev.read(buf.data(), block, 1);
    if (rc < 0) {
        return rc;
    }
    memcpy(buf.data() + off, &f->groups[group], sizeof(group_desc));
    return f->dev.write(buf.data(), block, 1);
}

// --- block allocation ---------------------------------------------------

// Claim up to `want` consecutive free blocks, preferring the group `goal` falls
// in so a growing file stays contiguous. Contiguity is what keeps the extent
// count low, which matters because the tree is capped at depth 1.
//
// Returns the first block in *out and how many were actually claimed in *got
// (at least 1 on success).
int block_alloc(fs *f, uint64_t goal, uint32_t want, uint64_t *out, uint32_t *got)
{
    if (want == 0) {
        return -EINVAL;
    }

    SCOPE_LOCK(f->alloc_lock);

    if (le32(f->sb.s_free_blocks_count_lo) == 0) {
        return -ENOSPC;
    }

    uint32_t start_group = 0;
    if (goal >= f->first_data_block && goal < f->block_count) {
        start_group = static_cast<uint32_t>((goal - f->first_data_block) /
                                            f->blocks_per_group);
    }

    scratch bm(f->block_size);
    if (!bm) {
        return -ENOMEM;
    }

    for (uint32_t n = 0; n < f->group_count; n++) {
        const uint32_t g = (start_group + n) % f->group_count;
        if (le16(f->groups[g].bg_free_blocks_count_lo) == 0) {
            continue;
        }

        const uint64_t bitmap_block = le32(f->groups[g].bg_block_bitmap_lo);
        int rc = f->dev.read(bm.data(), bitmap_block, 1);
        if (rc < 0) {
            return rc;
        }

        const uint32_t limit = blocks_in_group(f, g);
        const uint64_t group_first =
            f->first_data_block + static_cast<uint64_t>(g) * f->blocks_per_group;

        // Within the goal's own group, start scanning at the goal itself so a
        // sequential writer keeps extending the same run.
        uint32_t scan_from = 0;
        if (g == start_group && goal >= group_first &&
            goal - group_first < limit) {
            scan_from = static_cast<uint32_t>(goal - group_first);
        }

        for (int pass = 0; pass < 2; pass++) {
            const uint32_t begin = (pass == 0) ? scan_from : 0;
            const uint32_t end = (pass == 0) ? limit : scan_from;

            for (uint32_t i = begin; i < end; i++) {
                if (bit_test(bm.data(), i)) {
                    continue;
                }
                // Extend the run as far as it goes, capped by `want` and the
                // group's own end.
                uint32_t run = 1;
                while (run < want && i + run < limit &&
                       !bit_test(bm.data(), i + run)) {
                    run++;
                }

                for (uint32_t k = 0; k < run; k++) {
                    bit_set(bm.data(), i + k);
                }

                rc = f->dev.write(bm.data(), bitmap_block, 1);
                if (rc < 0) {
                    return rc;
                }

                f->groups[g].bg_free_blocks_count_lo = le16(static_cast<uint16_t>(
                    le16(f->groups[g].bg_free_blocks_count_lo) - run));
                f->sb.s_free_blocks_count_lo = le32(
                    le32(f->sb.s_free_blocks_count_lo) - run);

                rc = gd_write(f, g);
                if (rc < 0) {
                    return rc;
                }
                rc = sb_write(f);
                if (rc < 0) {
                    return rc;
                }

                *out = group_first + i;
                *got = run;
                return 0;
            }
            if (scan_from == 0) {
                break;      // pass 1 would repeat pass 0
            }
        }
    }
    return -ENOSPC;
}

int block_free(fs *f, uint64_t block, uint32_t count)
{
    if (count == 0) {
        return 0;
    }
    if (block < f->first_data_block || block + count > f->block_count) {
        printf("miniext: refusing to free out-of-range blocks %lu+%u\n",
               (unsigned long)block, count);
        return -EINVAL;
    }

    SCOPE_LOCK(f->alloc_lock);

    scratch bm(f->block_size);
    if (!bm) {
        return -ENOMEM;
    }

    while (count) {
        const uint32_t g = static_cast<uint32_t>((block - f->first_data_block) /
                                                 f->blocks_per_group);
        const uint64_t group_first =
            f->first_data_block + static_cast<uint64_t>(g) * f->blocks_per_group;
        const uint32_t idx = static_cast<uint32_t>(block - group_first);

        uint32_t here = count;
        if (idx + here > blocks_in_group(f, g)) {
            here = blocks_in_group(f, g) - idx;
        }

        const uint64_t bitmap_block = le32(f->groups[g].bg_block_bitmap_lo);
        int rc = f->dev.read(bm.data(), bitmap_block, 1);
        if (rc < 0) {
            return rc;
        }
        for (uint32_t k = 0; k < here; k++) {
            if (!bit_test(bm.data(), idx + k)) {
                printf("miniext: double free of block %lu\n",
                       (unsigned long)(block + k));
                return -EIO;
            }
            bit_clear(bm.data(), idx + k);
        }
        rc = f->dev.write(bm.data(), bitmap_block, 1);
        if (rc < 0) {
            return rc;
        }

        f->groups[g].bg_free_blocks_count_lo = le16(static_cast<uint16_t>(
            le16(f->groups[g].bg_free_blocks_count_lo) + here));
        f->sb.s_free_blocks_count_lo = le32(
            le32(f->sb.s_free_blocks_count_lo) + here);

        rc = gd_write(f, g);
        if (rc < 0) {
            return rc;
        }
        rc = sb_write(f);
        if (rc < 0) {
            return rc;
        }

        block += here;
        count -= here;
    }
    return 0;
}

// --- inode allocation ---------------------------------------------------

int inode_alloc(fs *f, bool is_dir, uint32_t *out)
{
    SCOPE_LOCK(f->alloc_lock);

    if (le32(f->sb.s_free_inodes_count) == 0) {
        return -ENOSPC;
    }

    scratch bm(f->block_size);
    if (!bm) {
        return -ENOMEM;
    }

    for (uint32_t g = 0; g < f->group_count; g++) {
        if (le16(f->groups[g].bg_free_inodes_count_lo) == 0) {
            continue;
        }

        const uint64_t bitmap_block = le32(f->groups[g].bg_inode_bitmap_lo);
        int rc = f->dev.read(bm.data(), bitmap_block, 1);
        if (rc < 0) {
            return rc;
        }

        for (uint32_t i = 0; i < f->inodes_per_group; i++) {
            if (bit_test(bm.data(), i)) {
                continue;
            }
            const uint32_t ino = g * f->inodes_per_group + i + 1;
            // Inodes below s_first_ino are reserved (root, journal, ...).
            if (ino < le32(f->sb.s_first_ino) && ino != ROOT_INO) {
                continue;
            }

            bit_set(bm.data(), i);
            rc = f->dev.write(bm.data(), bitmap_block, 1);
            if (rc < 0) {
                return rc;
            }

            f->groups[g].bg_free_inodes_count_lo = le16(static_cast<uint16_t>(
                le16(f->groups[g].bg_free_inodes_count_lo) - 1));
            f->sb.s_free_inodes_count = le32(le32(f->sb.s_free_inodes_count) - 1);
            if (is_dir) {
                f->groups[g].bg_used_dirs_count_lo = le16(static_cast<uint16_t>(
                    le16(f->groups[g].bg_used_dirs_count_lo) + 1));
            }

            rc = gd_write(f, g);
            if (rc < 0) {
                return rc;
            }
            rc = sb_write(f);
            if (rc < 0) {
                return rc;
            }

            *out = ino;
            return 0;
        }
    }
    return -ENOSPC;
}

int inode_free(fs *f, uint32_t ino, bool was_dir)
{
    if (ino == 0 || ino > le32(f->sb.s_inodes_count)) {
        return -EINVAL;
    }

    SCOPE_LOCK(f->alloc_lock);

    const uint32_t g = (ino - 1) / f->inodes_per_group;
    const uint32_t i = (ino - 1) % f->inodes_per_group;

    scratch bm(f->block_size);
    if (!bm) {
        return -ENOMEM;
    }

    const uint64_t bitmap_block = le32(f->groups[g].bg_inode_bitmap_lo);
    int rc = f->dev.read(bm.data(), bitmap_block, 1);
    if (rc < 0) {
        return rc;
    }
    if (!bit_test(bm.data(), i)) {
        printf("miniext: double free of inode %u\n", ino);
        return -EIO;
    }
    bit_clear(bm.data(), i);
    rc = f->dev.write(bm.data(), bitmap_block, 1);
    if (rc < 0) {
        return rc;
    }

    f->groups[g].bg_free_inodes_count_lo = le16(static_cast<uint16_t>(
        le16(f->groups[g].bg_free_inodes_count_lo) + 1));
    f->sb.s_free_inodes_count = le32(le32(f->sb.s_free_inodes_count) + 1);
    if (was_dir && le16(f->groups[g].bg_used_dirs_count_lo) > 0) {
        f->groups[g].bg_used_dirs_count_lo = le16(static_cast<uint16_t>(
            le16(f->groups[g].bg_used_dirs_count_lo) - 1));
    }

    rc = gd_write(f, g);
    if (rc < 0) {
        return rc;
    }
    return sb_write(f);
}

} // namespace miniext
