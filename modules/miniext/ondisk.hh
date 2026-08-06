/*
 * miniext: on-disk ext4 structures.
 *
 * Only the subset scripts/mkdata.sh produces is described here. Everything is
 * little-endian on disk; both architectures miniOSv targets are little-endian,
 * so the accessors below are identity conversions -- they exist to mark which
 * fields come off the disk rather than to swap bytes.
 *
 * Field offsets follow the ext4 on-disk layout. The static_asserts at the end
 * are the guard against a mis-declared field silently shifting everything
 * after it.
 */

#ifndef MINIEXT_ONDISK_HH
#define MINIEXT_ONDISK_HH

#include <cstddef>
#include <cstdint>

namespace miniext {

inline uint16_t le16(uint16_t v) { return v; }
inline uint32_t le32(uint32_t v) { return v; }

// The superblock lives at byte offset 1024 and is 1024 bytes long, whatever
// the block size. Fields past what we use are left as padding.
struct superblock {
    uint32_t s_inodes_count;            // 0x00
    uint32_t s_blocks_count_lo;         // 0x04
    uint32_t s_r_blocks_count_lo;       // 0x08
    uint32_t s_free_blocks_count_lo;    // 0x0C
    uint32_t s_free_inodes_count;       // 0x10
    uint32_t s_first_data_block;        // 0x14
    uint32_t s_log_block_size;          // 0x18  block size = 1024 << this
    uint32_t s_log_cluster_size;        // 0x1C
    uint32_t s_blocks_per_group;        // 0x20
    uint32_t s_clusters_per_group;      // 0x24
    uint32_t s_inodes_per_group;        // 0x28
    uint32_t s_mtime;                   // 0x2C
    uint32_t s_wtime;                   // 0x30
    uint16_t s_mnt_count;               // 0x34
    uint16_t s_max_mnt_count;           // 0x36
    uint16_t s_magic;                   // 0x38  0xEF53
    uint16_t s_state;                   // 0x3A
    uint16_t s_errors;                  // 0x3C
    uint16_t s_minor_rev_level;         // 0x3E
    uint32_t s_lastcheck;               // 0x40
    uint32_t s_checkinterval;           // 0x44
    uint32_t s_creator_os;              // 0x48
    uint32_t s_rev_level;               // 0x4C
    uint16_t s_def_resuid;              // 0x50
    uint16_t s_def_resgid;              // 0x52
    uint32_t s_first_ino;               // 0x54
    uint16_t s_inode_size;              // 0x58
    uint16_t s_block_group_nr;          // 0x5A
    uint32_t s_feature_compat;          // 0x5C
    uint32_t s_feature_incompat;        // 0x60
    uint32_t s_feature_ro_compat;       // 0x64
    uint8_t  s_uuid[16];                // 0x68
    char     s_volume_name[16];         // 0x78
    char     s_last_mounted[64];        // 0x88
    uint32_t s_algorithm_usage_bitmap;  // 0xC8
    uint8_t  s_prealloc_blocks;         // 0xCC
    uint8_t  s_prealloc_dir_blocks;     // 0xCD
    uint16_t s_reserved_gdt_blocks;     // 0xCE
    uint8_t  s_journal_uuid[16];        // 0xD0
    uint32_t s_journal_inum;            // 0xE0
    uint32_t s_journal_dev;             // 0xE4
    uint32_t s_last_orphan;             // 0xE8
    uint32_t s_hash_seed[4];            // 0xEC
    uint8_t  s_def_hash_version;        // 0xFC
    uint8_t  s_jnl_backup_type;         // 0xFD
    uint16_t s_desc_size;               // 0xFE  group descriptor size
    uint8_t  s_padding[1024 - 0x100];   // 0x100
} __attribute__((packed));

// 32 bytes without the 64bit feature, which mkdata.sh disables.
struct group_desc {
    uint32_t bg_block_bitmap_lo;        // 0x00
    uint32_t bg_inode_bitmap_lo;        // 0x04
    uint32_t bg_inode_table_lo;         // 0x08
    uint16_t bg_free_blocks_count_lo;   // 0x0C
    uint16_t bg_free_inodes_count_lo;   // 0x0E
    uint16_t bg_used_dirs_count_lo;     // 0x10
    uint16_t bg_flags;                  // 0x12
    uint32_t bg_exclude_bitmap_lo;      // 0x14
    uint16_t bg_block_bitmap_csum_lo;   // 0x18
    uint16_t bg_inode_bitmap_csum_lo;   // 0x1A
    uint16_t bg_itable_unused_lo;       // 0x1C
    uint16_t bg_checksum;               // 0x1E
} __attribute__((packed));

struct inode {
    uint16_t i_mode;                    // 0x00
    uint16_t i_uid;                     // 0x02
    uint32_t i_size_lo;                 // 0x04
    uint32_t i_atime;                   // 0x08
    uint32_t i_ctime;                   // 0x0C
    uint32_t i_mtime;                   // 0x10
    uint32_t i_dtime;                   // 0x14
    uint16_t i_gid;                     // 0x18
    uint16_t i_links_count;             // 0x1A
    uint32_t i_blocks_lo;               // 0x1C  in 512-byte units
    uint32_t i_flags;                   // 0x20
    uint32_t i_osd1;                    // 0x24
    uint8_t  i_block[60];               // 0x28  extent tree root lives here
    uint32_t i_generation;              // 0x64
    uint32_t i_file_acl_lo;             // 0x68
    uint32_t i_size_high;               // 0x6C
    uint32_t i_obso_faddr;              // 0x70
    uint8_t  i_osd2[12];                // 0x74
    uint16_t i_extra_isize;             // 0x80
} __attribute__((packed));

// i_mode type bits
static const uint16_t S_IFMT_MASK = 0xF000;
static const uint16_t MODE_FIFO   = 0x1000;
static const uint16_t MODE_CHR    = 0x2000;
static const uint16_t MODE_DIR    = 0x4000;
static const uint16_t MODE_BLK    = 0x6000;
static const uint16_t MODE_REG    = 0x8000;
static const uint16_t MODE_LNK    = 0xA000;

// i_flags
static const uint32_t INODE_FL_EXTENTS = 0x00080000;
static const uint32_t INODE_FL_HUGE    = 0x00040000;

// Extent tree. The root sits in inode.i_block; interior and leaf nodes each
// occupy one filesystem block. depth 0 means the entries are leaves.
static const uint16_t EXTENT_MAGIC = 0xF30A;

struct extent_header {
    uint16_t eh_magic;                  // 0x00  0xF30A
    uint16_t eh_entries;                // 0x02
    uint16_t eh_max;                    // 0x04
    uint16_t eh_depth;                  // 0x06
    uint32_t eh_generation;             // 0x08
} __attribute__((packed));

struct extent {                         // depth == 0
    uint32_t ee_block;                  // 0x00  first file block covered
    uint16_t ee_len;                    // 0x04  >32768 => uninitialised
    uint16_t ee_start_hi;               // 0x06
    uint32_t ee_start_lo;               // 0x08
} __attribute__((packed));

struct extent_idx {                     // depth > 0
    uint32_t ei_block;                  // 0x00
    uint32_t ei_leaf_lo;                // 0x04
    uint16_t ei_leaf_hi;                // 0x08
    uint16_t ei_unused;                 // 0x0A
} __attribute__((packed));

static const uint16_t EXTENT_INIT_MAX_LEN = 32768;

inline uint64_t extent_start(const extent *e)
{
    return (static_cast<uint64_t>(le16(e->ee_start_hi)) << 32) | le32(e->ee_start_lo);
}

inline uint64_t extent_idx_leaf(const extent_idx *ix)
{
    return (static_cast<uint64_t>(le16(ix->ei_leaf_hi)) << 32) | le32(ix->ei_leaf_lo);
}

// A length above 32768 marks an uninitialised extent, which reads as zeros.
inline uint16_t extent_len(const extent *e)
{
    uint16_t l = le16(e->ee_len);
    return l > EXTENT_INIT_MAX_LEN ? l - EXTENT_INIT_MAX_LEN : l;
}

inline bool extent_is_uninit(const extent *e)
{
    return le16(e->ee_len) > EXTENT_INIT_MAX_LEN;
}

// Directory entries are packed linearly into a directory's data blocks. An
// entry never straddles a block boundary, and rec_len of the last entry in a
// block runs to the end of that block. inode == 0 marks a free slot.
struct dir_entry {
    uint32_t inode;                     // 0x00
    uint16_t rec_len;                   // 0x04
    uint8_t  name_len;                  // 0x06
    uint8_t  file_type;                 // 0x07
    char     name[];                    // 0x08
} __attribute__((packed));

static const size_t DIR_ENTRY_HEADER = 8;

// file_type values (the filetype feature is on, so these are meaningful)
static const uint8_t FT_UNKNOWN = 0;
static const uint8_t FT_REG     = 1;
static const uint8_t FT_DIR     = 2;

// Feature bits. Anything outside the supported masks is rejected at mount:
// guessing at an unknown layout is how a filesystem silently corrupts data.
static const uint32_t INCOMPAT_FILETYPE    = 0x0002;
static const uint32_t INCOMPAT_RECOVER     = 0x0004;
static const uint32_t INCOMPAT_META_BG     = 0x0010;
static const uint32_t INCOMPAT_EXTENTS     = 0x0040;
static const uint32_t INCOMPAT_64BIT       = 0x0080;
static const uint32_t INCOMPAT_FLEX_BG     = 0x0200;
static const uint32_t INCOMPAT_INLINE_DATA = 0x8000;

static const uint32_t RO_COMPAT_SPARSE_SUPER  = 0x0001;
static const uint32_t RO_COMPAT_LARGE_FILE    = 0x0002;
static const uint32_t RO_COMPAT_HUGE_FILE     = 0x0008;
static const uint32_t RO_COMPAT_GDT_CSUM      = 0x0010;
static const uint32_t RO_COMPAT_DIR_NLINK     = 0x0020;
static const uint32_t RO_COMPAT_EXTRA_ISIZE   = 0x0040;
static const uint32_t RO_COMPAT_METADATA_CSUM = 0x0400;

// What mkdata.sh leaves enabled, and therefore what we must understand.
static const uint32_t SUPPORTED_INCOMPAT =
    INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG;

static const uint32_t SUPPORTED_RO_COMPAT =
    RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | RO_COMPAT_HUGE_FILE |
    RO_COMPAT_DIR_NLINK | RO_COMPAT_EXTRA_ISIZE;

static const uint16_t SUPERBLOCK_MAGIC = 0xEF53;
static const uint64_t SUPERBLOCK_OFFSET = 1024;
static const uint32_t ROOT_INO = 2;

static_assert(sizeof(superblock) == 1024, "superblock must be 1024 bytes");
static_assert(offsetof(superblock, s_magic) == 0x38, "s_magic offset");
static_assert(offsetof(superblock, s_inode_size) == 0x58, "s_inode_size offset");
static_assert(offsetof(superblock, s_feature_incompat) == 0x60, "s_feature_incompat offset");
static_assert(offsetof(superblock, s_desc_size) == 0xFE, "s_desc_size offset");
static_assert(sizeof(group_desc) == 32, "group_desc must be 32 bytes");
static_assert(offsetof(inode, i_block) == 0x28, "i_block offset");
static_assert(offsetof(inode, i_size_high) == 0x6C, "i_size_high offset");
static_assert(offsetof(inode, i_extra_isize) == 0x80, "i_extra_isize offset");
static_assert(sizeof(extent_header) == 12, "extent_header must be 12 bytes");
static_assert(sizeof(extent) == 12, "extent must be 12 bytes");
static_assert(sizeof(extent_idx) == 12, "extent_idx must be 12 bytes");

} // namespace miniext

#endif /* MINIEXT_ONDISK_HH */
