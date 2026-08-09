/*
 * miniext mount: read and validate the superblock, load the group descriptors.
 *
 * Validation is deliberately strict. An ext4 image can carry features that
 * change the on-disk layout in ways this implementation does not know about,
 * and guessing at one of those is how a filesystem corrupts data silently. Any
 * unsupported incompat or ro_compat bit is a hard mount failure naming the bit.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "internal.hh"

namespace miniext {

namespace {
fs g_fs;

struct feature_name {
    uint32_t bit;
    const char *name;
};

const feature_name incompat_names[] = {
    {0x0001, "compression"}, {INCOMPAT_FILETYPE, "filetype"},
    {INCOMPAT_RECOVER, "needs_recovery"}, {0x0008, "journal_dev"},
    {INCOMPAT_META_BG, "meta_bg"}, {INCOMPAT_EXTENTS, "extent"},
    {INCOMPAT_64BIT, "64bit"}, {0x0100, "mmp"},
    {INCOMPAT_FLEX_BG, "flex_bg"}, {0x0400, "ea_inode"},
    {0x1000, "dirdata"}, {0x2000, "csum_seed"}, {0x4000, "largedir"},
    {INCOMPAT_INLINE_DATA, "inline_data"}, {0x10000, "encrypt"},
    {0x20000, "casefold"}, {0, nullptr},
};

const feature_name ro_compat_names[] = {
    {RO_COMPAT_SPARSE_SUPER, "sparse_super"}, {RO_COMPAT_LARGE_FILE, "large_file"},
    {0x0004, "btree_dir"}, {RO_COMPAT_HUGE_FILE, "huge_file"},
    {RO_COMPAT_GDT_CSUM, "uninit_bg"}, {RO_COMPAT_DIR_NLINK, "dir_nlink"},
    {RO_COMPAT_EXTRA_ISIZE, "extra_isize"}, {0x0100, "quota"},
    {0x0200, "bigalloc"}, {RO_COMPAT_METADATA_CSUM, "metadata_csum"},
    {0x1000, "readonly"}, {0x2000, "project"}, {0, nullptr},
};

void report_unsupported(const char *kind, uint32_t bits, const feature_name *names)
{
    printf("miniext: unsupported %s features 0x%x:", kind, bits);
    for (const feature_name *n = names; n->name; n++) {
        if (bits & n->bit) {
            printf(" %s", n->name);
            bits &= ~n->bit;
        }
    }
    if (bits) {
        printf(" unknown(0x%x)", bits);
    }
    printf("\n");
    printf("miniext: rebuild the image with scripts/mkdata.sh\n");
}
} // namespace

fs *get_fs() { return &g_fs; }

bool is_mounted() { return g_fs.mounted; }

std::string mount_point() { return g_fs.mounted ? g_fs.mount_point : std::string(); }

int mount(int nvme_id, const char *mount_point)
{
    fs *f = &g_fs;

    if (f->mounted) {
        printf("miniext: already mounted at %s\n", f->mount_point.c_str());
        return -EBUSY;
    }
    if (!mount_point || mount_point[0] != '/') {
        printf("miniext: mount point must be an absolute path\n");
        return -EINVAL;
    }

    // Creates the one I/O queue. Until set_block_size() below, the device
    // addresses raw LBAs -- the superblock has to be read before its own
    // s_log_block_size can say how big a filesystem block is.
    int rc = f->dev.open(nvme_id);
    if (rc < 0) {
        return rc;
    }

    {
        const uint32_t lba = f->dev.lba_size();
        if (SUPERBLOCK_OFFSET % lba != 0 || sizeof(superblock) % lba != 0) {
            printf("miniext: %u-byte LBAs do not divide the superblock offset\n", lba);
            return -EINVAL;
        }
        scratch buf(sizeof(superblock));
        if (!buf) {
            return -ENOMEM;
        }
        rc = f->dev.read(buf.data(), SUPERBLOCK_OFFSET / lba,
                         sizeof(superblock) / lba);
        if (rc < 0) {
            printf("miniext: could not read the superblock\n");
            return rc;
        }
        memcpy(&f->sb, buf.data(), sizeof(superblock));
    }

    if (le16(f->sb.s_magic) != SUPERBLOCK_MAGIC) {
        printf("miniext: bad magic 0x%x (expected 0x%x) - not an ext filesystem\n",
               le16(f->sb.s_magic), SUPERBLOCK_MAGIC);
        return -EINVAL;
    }

    uint32_t incompat = le32(f->sb.s_feature_incompat) & ~SUPPORTED_INCOMPAT;
    if (incompat) {
        report_unsupported("incompat", incompat, incompat_names);
        return -ENOTSUP;
    }
    // ro_compat normally means "mountable read-only", but miniext mounts to
    // write, so an unknown bit here is just as disqualifying.
    uint32_t ro = le32(f->sb.s_feature_ro_compat) & ~SUPPORTED_RO_COMPAT;
    if (ro) {
        report_unsupported("ro_compat", ro, ro_compat_names);
        return -ENOTSUP;
    }
    if (!(le32(f->sb.s_feature_incompat) & INCOMPAT_EXTENTS)) {
        printf("miniext: image has no extent feature; the legacy indirect block "
               "map is not implemented\n");
        return -ENOTSUP;
    }

    f->block_size = 1024u << le32(f->sb.s_log_block_size);
    f->inode_size = le16(f->sb.s_inode_size);
    f->inodes_per_group = le32(f->sb.s_inodes_per_group);
    f->blocks_per_group = le32(f->sb.s_blocks_per_group);
    f->block_count = le32(f->sb.s_blocks_count_lo);
    f->first_data_block = le32(f->sb.s_first_data_block);

    if (f->block_size < 1024 || f->block_size > 65536 ||
        (f->block_size & (f->block_size - 1)) != 0) {
        printf("miniext: implausible block size %u\n", f->block_size);
        return -EINVAL;
    }
    if (f->inode_size < 128 || f->inode_size > f->block_size ||
        (f->inode_size & (f->inode_size - 1)) != 0) {
        printf("miniext: implausible inode size %u\n", f->inode_size);
        return -EINVAL;
    }
    if (f->inodes_per_group == 0 || f->blocks_per_group == 0) {
        printf("miniext: zero inodes or blocks per group\n");
        return -EINVAL;
    }

    // Switch the device's addressing unit to the filesystem block size. This
    // reuses the existing queue rather than re-opening.
    rc = f->dev.set_block_size(f->block_size);
    if (rc < 0) {
        return rc;
    }

    f->group_count = static_cast<uint32_t>(
        (f->block_count - f->first_data_block + f->blocks_per_group - 1) /
        f->blocks_per_group);

    uint16_t desc_size = le16(f->sb.s_desc_size);
    if (desc_size != 0 && desc_size != sizeof(group_desc)) {
        // Only the 64bit feature grows descriptors, and that is rejected above.
        printf("miniext: unexpected group descriptor size %u (expected %zu)\n",
               desc_size, sizeof(group_desc));
        return -ENOTSUP;
    }

    // The group descriptor table starts in the block after the superblock.
    const uint64_t gdt_block = f->first_data_block + 1;
    const uint32_t per_block = f->block_size / sizeof(group_desc);
    const uint32_t gdt_blocks = (f->group_count + per_block - 1) / per_block;

    f->groups.resize(f->group_count);
    {
        scratch buf(f->block_size);
        if (!buf) {
            return -ENOMEM;
        }
        for (uint32_t b = 0; b < gdt_blocks; b++) {
            rc = f->dev.read(buf.data(), gdt_block + b, 1);
            if (rc < 0) {
                printf("miniext: could not read group descriptor block %u\n", b);
                return rc;
            }
            uint32_t first = b * per_block;
            uint32_t n = f->group_count - first;
            if (n > per_block) {
                n = per_block;
            }
            memcpy(&f->groups[first], buf.data(), n * sizeof(group_desc));
        }
    }

    f->mount_point = mount_point;
    // Strip a trailing slash so "/db/" and "/db" behave alike.
    while (f->mount_point.size() > 1 && f->mount_point.back() == '/') {
        f->mount_point.pop_back();
    }
    f->mounted = true;

    printf("miniext: mounted nvme%d at %s: %u-byte blocks, %lu blocks, "
           "%u inodes, %u groups\n",
           nvme_id, f->mount_point.c_str(), f->block_size,
           (unsigned long)f->block_count, le32(f->sb.s_inodes_count),
           f->group_count);
    return 0;
}

int umount()
{
    fs *f = &g_fs;
    if (!f->mounted) {
        return -EINVAL;
    }
    f->dev.flush();
    f->dev.close();
    f->groups.clear();
    f->mounted = false;
    return 0;
}

int info(fs_info *out)
{
    fs *f = &g_fs;
    if (!f->mounted) {
        return -EINVAL;
    }
    out->block_size = f->block_size;
    out->block_count = f->block_count;
    out->free_blocks = le32(f->sb.s_free_blocks_count_lo);
    out->inode_count = le32(f->sb.s_inodes_count);
    out->free_inodes = le32(f->sb.s_free_inodes_count);
    out->group_count = f->group_count;
    return 0;
}

} // namespace miniext
