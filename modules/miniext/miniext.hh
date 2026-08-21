/*
 * miniext: a minimal ext4-compatible filesystem for miniOSv.
 *
 * The application calls these functions directly.
 * No VFS, no file-descriptor table, and no libc file I/O involved.
 *
 * miniext interacts with the NVMe itself rather than through a block-device
 * abstraction: mount() takes the id of a controller registered by
 * drivers/nvme.cc (0 is the boot disk, 1 the second one attached with
 * run.py --emulated-nvme).
 *
 * The supported on-disk subset is whatever scripts/mkdata.sh produces; mount()
 * refuses anything else rather than guessing. See ondisk.hh.
 */

#ifndef MINIEXT_HH
#define MINIEXT_HH

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace mem { struct store; }

namespace miniext {

struct file;

// Negative return values are -errno throughout.

// Mount the namespace of NVMe controller `nvme_id` at `mount_point` (an
// absolute path such as "/db"). Only one filesystem may be mounted at a time.
int mount(int nvme_id, const char *mount_point);
int umount();
bool is_mounted();

// Mountpoint. Empty when not mounted.
// A relative path has no meaning here -- there is no working directory -- so
// callers that receive one resolve it against this.
std::string mount_point();

// Paths are absolute and must start with the mount point.
static const int O_RD     = 0x1;
static const int O_WR     = 0x2;
static const int O_RDWR   = O_RD | O_WR;
static const int O_CREATE = 0x4;   // create if absent
static const int O_EXCL   = 0x8;   // with O_CREATE, fail if present
static const int O_TRUNC  = 0x10;  // truncate to zero on open

file *open(const char *path, int flags, int *err = nullptr);
void close(file *f);

// Positional read/write. Return bytes moved, or -errno; pread returns 0 at EOF.
// A read spanning a hole yields zeros there; a write past EOF extends the file,
// leaving any skipped range as a hole.
//
// Concurrency is POSIX-shaped and no stronger: reads on one file run in
// parallel with each other, a write excludes readers and other writers of that
// same file, and different files never block each other.
int64_t pread(file *f, void *buf, size_t len, uint64_t offset);
int64_t pwrite(file *f, const void *buf, size_t len, uint64_t offset);

// async IO operations
struct aio;

aio *aread(file *f, void *buf, size_t len, uint64_t offset);
aio *awrite(file *f, const void *buf, size_t len, uint64_t offset);

// Whether it has landed, without waiting for it.
bool adone(aio *a);

// Wait for it, release it, and say how it went: bytes moved, or -errno.
int64_t await(aio *a);

int truncate(file *f, uint64_t new_size);

// Push the device's volatile write cache. Writes already returned are on the
// media afterwards.
int sync(file *f);
int sync();

uint64_t size(file *f);

// for the page cache
mem::store *store_open(file *f);
void store_close(mem::store *s);

// Metadata without opening.
bool exists(const char *path);
bool is_directory(const char *path);
int  file_size(const char *path, uint64_t *out);

// Directory listing. `cb` is called once per entry with the entry name and
// whether it is a directory; "." and ".." are skipped.
int list(const char *path, const std::function<void(const char *, bool)> &cb);

// Namespace mutation.
int unlink(const char *path);
int rename(const char *from, const char *to);
int mkdir(const char *path);
int rmdir(const char *path);

// Geometry, for diagnostics.
struct fs_info {
    uint32_t block_size;
    uint64_t block_count;
    uint64_t free_blocks;
    uint32_t inode_count;
    uint32_t free_inodes;
    uint32_t group_count;
};
int info(fs_info *out);

// --- raw namespaces ------------------------------------------------------
//
// An NVMe namespace read as one flat file, with no filesystem on it.
// Used to attach images with a single file.
namespace raw {

struct device;

// `nvme_id` numbers the controllers as mount() does: 0 is the boot disk, then
// the --emulated-nvme drives in the order they were given.
device *open(int nvme_id, int *err = nullptr);
void close(device *d);

// The namespace size in bytes. The host file is rounded up to a whole number
// of LBAs, so this can exceed the original file by up to one LBA.
uint64_t size(device *d);

// Bytes read, 0 at end of namespace, or -errno.
int64_t pread(device *d, void *buf, size_t len, uint64_t offset);

} // namespace raw

} // namespace miniext

#endif /* MINIEXT_HH */
