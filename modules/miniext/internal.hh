/*
 * miniext internals shared between the translation units. Not for app use.
 */

#ifndef MINIEXT_INTERNAL_HH
#define MINIEXT_INTERNAL_HH

#include <cstdint>
#include <memory>
#include <vector>
#include <osv/contiguous_alloc.hh>
#include <osv/mutex.h>
#include <osv/rwlock.h>

#include "miniext.hh"
#include "ondisk.hh"

namespace nvme { class io_queue_pair; class nvme_driver; }

namespace miniext {

// --- device -------------------------------------------------------------
//
// The block layer, folded into the filesystem rather than sitting under it as
// a separate abstraction. It turns filesystem block numbers into the byte
// offsets the driver wants.
//
// Note the driver's submit_request() takes a BYTE offset and BYTE length
// despite its parameter names, rings the doorbell itself, and returns 1 on
// success / 0 when the submission queue was full and nothing was queued.
//
// One I/O queue per vCPU. A single io_queue_pair is not safe for concurrent
// submitters -- its SCOPE_LOCK is commented out (drivers/nvme-queue.cc:338) and
// the SQ tail advance is unsynchronised -- but separate queues are independent,
// which is how leanstore drives this driver too. A thread submits on the queue
// belonging to the CPU it is running on, and the per-queue lock is held only
// across submit_request(), never across the wait, so many requests are in
// flight at once.
class device {
public:
    int open(int nvme_id);
    int set_block_size(uint32_t block_size);
    void close();

    // Blocking for the caller; concurrent callers proceed in parallel.
    // `block` and `count` are in filesystem blocks.
    int read(void *buf, uint64_t block, uint32_t count);
    int write(const void *buf, uint64_t block, uint32_t count);
    int flush();

    uint64_t lba_count() const { return _lba_count; }
    uint32_t lba_size() const { return _lba_size; }
    size_t queue_count() const { return _queues ? _queues->size() : 0; }

    struct queue {
        nvme::io_queue_pair *q = nullptr;
        mutex lock;             // submission only, never held across the wait
    };

    // The queues belong to the controller, not to whoever opened it. They are
    // created on the first open() of a controller and shared by every device
    // addressing it afterwards -- the filesystem and a raw namespace reader can
    // hold the same controller with different block sizes, and opening one file
    // twice must not ask for a second set.
    //
    // Sharing is safe for the same reason concurrent callers are: each queue
    // has its own lock, held only across submission.
    //
    // It is also necessary. The driver allocates one MSI-X vector per queue and
    // never recycles a queue id (drivers/nvme.cc:413), so a second set would be
    // refused once the vectors ran out.
    using queue_set = std::vector<std::unique_ptr<queue>>;

private:
    static std::shared_ptr<queue_set> queues_for(int nvme_id,
                                                 nvme::nvme_driver *drv);
    int submit(void *buf, uint64_t block, uint32_t count, bool write);
    int bounce(void *buf, uint64_t block, uint32_t count, bool write);
    queue &pick();

    std::shared_ptr<queue_set> _queues;
    uint32_t _lba_size = 0;
    uint64_t _lba_count = 0;
    uint32_t _block_size = 0;
    uint32_t _lbas_per_block = 0;
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

    // Control path only: mount/umount, path resolution, directory mutation and
    // the open-inode table. Data-path reads and writes do not take it.
    mutex lock;

    // Bitmaps + group descriptors + superblock counters, which must move
    // together. Held only while claiming or releasing blocks and inodes, never
    // across a data transfer.
    mutex alloc_lock;
};

// --- open inodes --------------------------------------------------------
//
// One entry per inode that is currently open, shared by every handle onto it,
// so a reader and a writer see the same inode rather than private stale copies.
//
// `lock` gives POSIX-level behaviour and no more: pread takes it shared so
// reads on one file run in parallel, pwrite/truncate take it exclusive so a
// read never observes a half-applied write.
struct open_inode {
    uint32_t ino = 0;
    inode in;
    rwlock lock;
    int refs = 0;
};

open_inode *oi_get(fs *f, uint32_t ino, const inode *in);
void oi_put(fs *f, open_inode *oi);

fs *get_fs();

// --- metadata writeback -------------------------------------------------
//
// Every one of these lands on the device before it returns. There is no cache
// and no journal, so an interrupted sequence can leave the filesystem
// inconsistent -- the order things are written in is the only ordering
// guarantee there is. Allocate-then-link, and unlink-then-free.

int sb_write(fs *f);
int gd_write(fs *f, uint32_t group);

// --- allocators ---------------------------------------------------------
//
// Bitmap based. `goal` is a hint: allocation starts scanning at that block's
// group so a growing file stays contiguous, which is what keeps extent counts
// low enough for the depth limit below.

int block_alloc(fs *f, uint64_t goal, uint32_t want, uint64_t *out, uint32_t *got);
int block_free(fs *f, uint64_t block, uint32_t count);
int inode_alloc(fs *f, bool is_dir, uint32_t *out);
int inode_free(fs *f, uint32_t ino, bool was_dir);

// --- inode --------------------------------------------------------------

int inode_read(fs *f, uint32_t ino, inode *out);
int inode_write(fs *f, uint32_t ino, const inode *in);
void inode_init(fs *f, inode *in, uint16_t mode);
uint64_t inode_size(const inode *in);
void inode_set_size(inode *in, uint64_t size);
void inode_mark_deleted(inode *in);
bool inode_is_dir(const inode *in);
bool inode_is_reg(const inode *in);

// i_blocks counts 512-byte units, and includes extent-tree blocks.
void inode_add_blocks(fs *f, inode *in, int64_t delta_fs_blocks);

// --- extents ------------------------------------------------------------
//
// Map file block `fblock` to a physical block. Sets *phys to 0 for a hole (an
// unallocated range or an uninitialised extent), which reads as zeros.
// *run is the number of consecutive blocks that share the mapping, so callers
// can coalesce I/O.
int extent_lookup(fs *f, const inode *in, uint32_t fblock,
                  uint64_t *phys, uint32_t *run);

// Ensure file block `fblock` is backed, allocating up to `want` consecutive
// blocks if it is not. *phys/*run describe the resulting mapping.
int extent_map_write(fs *f, uint32_t ino, inode *in, uint32_t fblock,
                     uint32_t want, uint64_t *phys, uint32_t *run);

// Free every block from file block `from` onwards and trim the tree.
int extent_truncate(fs *f, uint32_t ino, inode *in, uint32_t from);

// --- directories --------------------------------------------------------

// Find `name` (not NUL-terminated, `name_len` bytes) in directory inode `dir`.
// Returns the inode number, or 0 if absent.
int dir_lookup(fs *f, const inode *dir, const char *name, size_t name_len,
               uint32_t *out_ino);

int dir_iterate(fs *f, const inode *dir,
                const std::function<bool(const char *, size_t, uint32_t, uint8_t)> &cb);

// Link `ino` into directory `dir` under `name`, growing the directory by a
// block if no existing record has room.
int dir_add(fs *f, uint32_t dir_ino, inode *dir, const char *name,
            size_t name_len, uint32_t ino, uint8_t file_type);

// Unlink `name`, merging its record into the preceding one.
int dir_remove(fs *f, uint32_t dir_ino, inode *dir, const char *name,
               size_t name_len);

// Split a path into its parent directory and final component.
int path_split(fs *f, const char *path, uint32_t *parent_ino, inode *parent,
               const char **name, size_t *name_len);

// --- path resolution ----------------------------------------------------

// Resolve an absolute path (including the mount point prefix) to an inode.
int path_resolve(fs *f, const char *path, uint32_t *out_ino, inode *out);

// Read `len` bytes at `offset` from an inode's data.
int64_t inode_pread(fs *f, const inode *in, void *buf, size_t len, uint64_t offset);

// Write `len` bytes at `offset`, allocating blocks as needed and extending
// i_size. The inode is written back before this returns.
int64_t inode_pwrite(fs *f, uint32_t ino, inode *in, const void *buf, size_t len,
                     uint64_t offset);

int inode_truncate(fs *f, uint32_t ino, inode *in, uint64_t new_size);

} // namespace miniext

#endif /* MINIEXT_INTERNAL_HH */
