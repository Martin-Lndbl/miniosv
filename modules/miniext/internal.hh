/*
 * miniext internals shared between the translation units. Not for app use.
 */

#ifndef MINIEXT_INTERNAL_HH
#define MINIEXT_INTERNAL_HH

#include <cstdint>
#include <vector>
#include <osv/contiguous_alloc.hh>
#include <osv/mutex.h>

#include "miniext.hh"
#include "ondisk.hh"

namespace nvme { class io_queue_pair; }

namespace miniext {

// --- device -------------------------------------------------------------
//
// The block layer, folded into the filesystem rather than sitting under it as
// a separate abstraction. It owns one NVMe I/O queue and turns filesystem
// block numbers into byte offsets for the driver.
//
// Note the driver's submit_request() takes a BYTE offset and BYTE length
// despite its parameter names, rings the doorbell itself, and returns 1 on
// success / 0 when the submission queue was full and nothing was queued.
class device {
public:
    int open(int nvme_id);
    int set_block_size(uint32_t block_size);
    void close();

    // Synchronous. `block` and `count` are in filesystem blocks.
    int read(void *buf, uint64_t block, uint32_t count);
    int write(const void *buf, uint64_t block, uint32_t count);
    int flush();

    uint64_t lba_count() const { return _lba_count; }
    uint32_t lba_size() const { return _lba_size; }

private:
    int submit(void *buf, uint64_t block, uint32_t count, bool write);

    nvme::io_queue_pair *_q = nullptr;
    uint32_t _lba_size = 0;
    uint64_t _lba_count = 0;
    uint32_t _block_size = 0;
    uint32_t _lbas_per_block = 0;
    mutex _lock;
};

// --- scratch buffers ----------------------------------------------------
//
// A block-sized staging buffer, allocated and freed around a single access.
// The device transfers whole blocks, so anything smaller -- the 1024-byte
// superblock, a 256-byte inode inside a 4096-byte block, a directory block we
// scan and discard, the partial block at the edge of an unaligned read -- lands
// here first and is then copied out. Whole-block file reads go straight into
// the caller's buffer instead.
//
// Block-sized and block-aligned makes it exactly one page, so a transfer needs
// only prp1: no prp2, no PRP-list page.
class scratch {
public:
    explicit scratch(uint32_t size)
        : _p(static_cast<uint8_t *>(
              memory::alloc_phys_contiguous_aligned(size, size))) {}
    ~scratch()
    {
        if (_p) {
            memory::free_phys_contiguous_aligned(_p);
        }
    }
    scratch(const scratch &) = delete;
    scratch &operator=(const scratch &) = delete;

    uint8_t *data() { return _p; }
    explicit operator bool() const { return _p != nullptr; }

private:
    uint8_t *_p;
};

// --- mounted filesystem -------------------------------------------------

struct fs {
    bool mounted = false;
    std::string mount_point;

    device dev;

    superblock sb;
    std::vector<group_desc> groups;

    uint32_t block_size = 0;
    uint32_t inode_size = 0;
    uint32_t inodes_per_group = 0;
    uint32_t blocks_per_group = 0;
    uint32_t group_count = 0;
    uint64_t block_count = 0;
    uint32_t first_data_block = 0;

    mutex lock;   // serialises everything above; see PLAN.md on concurrency
};

fs *get_fs();

// --- inode --------------------------------------------------------------

int inode_read(fs *f, uint32_t ino, inode *out);
uint64_t inode_size(const inode *in);
bool inode_is_dir(const inode *in);
bool inode_is_reg(const inode *in);

// --- extents ------------------------------------------------------------
//
// Map file block `fblock` to a physical block. Sets *phys to 0 for a hole (an
// unallocated range or an uninitialised extent), which reads as zeros.
// *run is the number of consecutive blocks that share the mapping, so callers
// can coalesce I/O.
int extent_lookup(fs *f, const inode *in, uint32_t fblock,
                  uint64_t *phys, uint32_t *run);

// --- directories --------------------------------------------------------

// Find `name` (not NUL-terminated, `name_len` bytes) in directory inode `dir`.
// Returns the inode number, or 0 if absent.
int dir_lookup(fs *f, const inode *dir, const char *name, size_t name_len,
               uint32_t *out_ino);

int dir_iterate(fs *f, const inode *dir,
                const std::function<bool(const char *, size_t, uint32_t, uint8_t)> &cb);

// --- path resolution ----------------------------------------------------

// Resolve an absolute path (including the mount point prefix) to an inode.
int path_resolve(fs *f, const char *path, uint32_t *out_ino, inode *out);

// Read `len` bytes at `offset` from an inode's data.
int64_t inode_pread(fs *f, const inode *in, void *buf, size_t len, uint64_t offset);

} // namespace miniext

#endif /* MINIEXT_INTERNAL_HH */
