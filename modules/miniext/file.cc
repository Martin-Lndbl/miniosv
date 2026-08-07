/*
 * miniext file operations: the public API from miniext.hh.
 *
 * There is no descriptor table -- the caller holds the pointer, which is the
 * point of talking to the filesystem directly instead of through a VFS.
 *
 * Locking, in the order things are taken:
 *
 *   fs::lock        control path only: path resolution, directory mutation,
 *                   and the open-inode table. Never held across a data
 *                   transfer.
 *   open_inode::lock  per file. pread takes it shared so reads on one file run
 *                   in parallel; pwrite and truncate take it exclusive.
 *   fs::alloc_lock  inside the allocators, briefly.
 *
 * That gives POSIX-shaped guarantees and no more: a read never sees a
 * half-applied write to the same file, writes to one file serialise, and
 * different files do not block each other at all.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "internal.hh"

namespace miniext {

struct file {
    open_inode *oi;
    int flags;
};

namespace {
// Every handle onto an inode shares one entry, so a reader and a writer see the
// same inode instead of private copies that drift apart.
std::unordered_map<uint32_t, open_inode *> g_open;
} // namespace

// Called with fs::lock held.
open_inode *oi_get(fs *f, uint32_t ino, const inode *in)
{
    (void)f;
    auto it = g_open.find(ino);
    if (it != g_open.end()) {
        it->second->refs++;
        return it->second;
    }
    auto *oi = new open_inode();
    oi->ino = ino;
    oi->in = *in;
    oi->refs = 1;
    g_open[ino] = oi;
    return oi;
}

// Called with fs::lock held.
void oi_put(fs *f, open_inode *oi)
{
    (void)f;
    if (--oi->refs == 0) {
        g_open.erase(oi->ino);
        delete oi;
    }
}

file *open(const char *path, int flags, int *err)
{
    int dummy;
    if (!err) {
        err = &dummy;
    }
    *err = 0;

    fs *f = get_fs();
    if (!f->mounted) {
        *err = -EINVAL;
        return nullptr;
    }

    open_inode *oi = nullptr;
    {
        SCOPE_LOCK(f->lock);

        uint32_t ino = 0;
        inode in;
        int rc = path_resolve(f, path, &ino, &in);

        if (rc == -ENOENT && (flags & O_CREATE)) {
            uint32_t parent_ino = 0;
            inode parent;
            const char *name = nullptr;
            size_t name_len = 0;
            rc = path_split(f, path, &parent_ino, &parent, &name, &name_len);
            if (rc < 0) {
                *err = rc;
                return nullptr;
            }
            if (!inode_is_dir(&parent)) {
                *err = -ENOTDIR;
                return nullptr;
            }

            rc = inode_alloc(f, false, &ino);
            if (rc < 0) {
                *err = rc;
                return nullptr;
            }
            inode_init(f, &in, MODE_REG | 0644);
            rc = inode_write(f, ino, &in);
            if (rc < 0) {
                inode_free(f, ino, false);
                *err = rc;
                return nullptr;
            }
            // Link last: an inode nobody points at is recoverable by fsck,
            // a dirent pointing at nothing is not.
            rc = dir_add(f, parent_ino, &parent, name, name_len, ino, FT_REG);
            if (rc < 0) {
                inode_free(f, ino, false);
                *err = rc;
                return nullptr;
            }
        } else if (rc == 0 && (flags & O_CREATE) && (flags & O_EXCL)) {
            *err = -EEXIST;
            return nullptr;
        } else if (rc < 0) {
            *err = rc;
            return nullptr;
        } else if (inode_is_dir(&in)) {
            *err = -EISDIR;
            return nullptr;
        } else if (!inode_is_reg(&in)) {
            *err = -EINVAL;
            return nullptr;
        }

        oi = oi_get(f, ino, &in);
    }

    if (flags & O_TRUNC) {
        WITH_LOCK(oi->lock.for_write()) {
            int rc = inode_truncate(f, oi->ino, &oi->in, 0);
            if (rc < 0) {
                SCOPE_LOCK(f->lock);
                oi_put(f, oi);
                *err = rc;
                return nullptr;
            }
        }
    }

    return new file{oi, flags};
}

void close(file *fp)
{
    if (!fp) {
        return;
    }
    fs *f = get_fs();
    {
        SCOPE_LOCK(f->lock);
        oi_put(f, fp->oi);
    }
    delete fp;
}

int64_t pread(file *fp, void *buf, size_t len, uint64_t offset)
{
    if (!fp) {
        return -EINVAL;
    }
    fs *f = get_fs();
    // Shared: concurrent readers of the same file proceed together, and readers
    // of other files are not involved at all.
    WITH_LOCK(fp->oi->lock.for_read()) {
        return inode_pread(f, &fp->oi->in, buf, len, offset);
    }
}

int64_t pwrite(file *fp, const void *buf, size_t len, uint64_t offset)
{
    if (!fp) {
        return -EINVAL;
    }
    if (!(fp->flags & O_WR)) {
        return -EBADF;
    }
    fs *f = get_fs();
    WITH_LOCK(fp->oi->lock.for_write()) {
        return inode_pwrite(f, fp->oi->ino, &fp->oi->in, buf, len, offset);
    }
}

int truncate(file *fp, uint64_t new_size)
{
    if (!fp) {
        return -EINVAL;
    }
    if (!(fp->flags & O_WR)) {
        return -EBADF;
    }
    fs *f = get_fs();
    WITH_LOCK(fp->oi->lock.for_write()) {
        return inode_truncate(f, fp->oi->ino, &fp->oi->in, new_size);
    }
}

int sync(file *fp)
{
    (void)fp;
    return sync();
}

int sync()
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    return f->dev.flush();
}

uint64_t size(file *fp)
{
    if (!fp) {
        return 0;
    }
    WITH_LOCK(fp->oi->lock.for_read()) {
        return inode_size(&fp->oi->in);
    }
}

bool exists(const char *path)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return false;
    }
    SCOPE_LOCK(f->lock);

    uint32_t ino = 0;
    inode in;
    return path_resolve(f, path, &ino, &in) == 0;
}

bool is_directory(const char *path)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return false;
    }
    SCOPE_LOCK(f->lock);

    uint32_t ino = 0;
    inode in;
    if (path_resolve(f, path, &ino, &in) < 0) {
        return false;
    }
    return inode_is_dir(&in);
}

int file_size(const char *path, uint64_t *out)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    SCOPE_LOCK(f->lock);

    uint32_t ino = 0;
    inode in;
    int rc = path_resolve(f, path, &ino, &in);
    if (rc < 0) {
        return rc;
    }
    // An open file's size lives in the shared inode, which is newer than what
    // is on disk if a write has not been followed by a close.
    auto it = g_open.find(ino);
    *out = (it != g_open.end()) ? inode_size(&it->second->in) : inode_size(&in);
    return 0;
}

int list(const char *path, const std::function<void(const char *, bool)> &cb)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    SCOPE_LOCK(f->lock);

    uint32_t ino = 0;
    inode in;
    int rc = path_resolve(f, path, &ino, &in);
    if (rc < 0) {
        return rc;
    }
    if (!inode_is_dir(&in)) {
        return -ENOTDIR;
    }

    return dir_iterate(f, &in,
        [&](const char *name, size_t len, uint32_t, uint8_t type) {
            if ((len == 1 && name[0] == '.') ||
                (len == 2 && name[0] == '.' && name[1] == '.')) {
                return true;
            }
            char buf[256];
            memcpy(buf, name, len);
            buf[len] = '\0';
            cb(buf, type == FT_DIR);
            return true;
        });
}

int unlink(const char *path)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    SCOPE_LOCK(f->lock);

    uint32_t parent_ino = 0;
    inode parent;
    const char *name = nullptr;
    size_t name_len = 0;
    int rc = path_split(f, path, &parent_ino, &parent, &name, &name_len);
    if (rc < 0) {
        return rc;
    }

    uint32_t ino = 0;
    rc = dir_lookup(f, &parent, name, name_len, &ino);
    if (rc < 0) {
        return rc;
    }
    if (ino == 0) {
        return -ENOENT;
    }

    inode in;
    rc = inode_read(f, ino, &in);
    if (rc < 0) {
        return rc;
    }
    if (inode_is_dir(&in)) {
        return -EISDIR;
    }

    // Unlink first, then release: a dirent pointing at a freed inode is far
    // worse than an inode nothing points at.
    rc = dir_remove(f, parent_ino, &parent, name, name_len);
    if (rc < 0) {
        return rc;
    }

    // Still open? POSIX keeps the data alive until the last close; we do not
    // implement that, so refuse rather than pull the blocks out from under it.
    if (g_open.find(ino) != g_open.end()) {
        printf("miniext: unlink of an open file is not supported (%s)\n", path);
        return -EBUSY;
    }

    rc = extent_truncate(f, ino, &in, 0);
    if (rc < 0) {
        return rc;
    }
    inode_mark_deleted(&in);
    inode_write(f, ino, &in);
    return inode_free(f, ino, false);
}

int rename(const char *from, const char *to)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    SCOPE_LOCK(f->lock);

    uint32_t from_parent = 0, to_parent = 0;
    inode fp_in, tp_in;
    const char *from_name = nullptr, *to_name = nullptr;
    size_t from_len = 0, to_len = 0;

    int rc = path_split(f, from, &from_parent, &fp_in, &from_name, &from_len);
    if (rc < 0) {
        return rc;
    }
    rc = path_split(f, to, &to_parent, &tp_in, &to_name, &to_len);
    if (rc < 0) {
        return rc;
    }

    uint32_t ino = 0;
    rc = dir_lookup(f, &fp_in, from_name, from_len, &ino);
    if (rc < 0) {
        return rc;
    }
    if (ino == 0) {
        return -ENOENT;
    }

    inode in;
    rc = inode_read(f, ino, &in);
    if (rc < 0) {
        return rc;
    }
    const uint8_t ftype = inode_is_dir(&in) ? FT_DIR : FT_REG;

    // POSIX renames over an existing target. Drop it first so dir_add does not
    // trip its duplicate check.
    uint32_t victim = 0;
    rc = dir_lookup(f, &tp_in, to_name, to_len, &victim);
    if (rc < 0) {
        return rc;
    }
    if (victim) {
        if (victim == ino) {
            return 0;               // same file, nothing to do
        }
        inode vin;
        rc = inode_read(f, victim, &vin);
        if (rc < 0) {
            return rc;
        }
        if (inode_is_dir(&vin)) {
            return -EISDIR;
        }
        if (g_open.find(victim) != g_open.end()) {
            return -EBUSY;
        }
        rc = dir_remove(f, to_parent, &tp_in, to_name, to_len);
        if (rc < 0) {
            return rc;
        }
        extent_truncate(f, victim, &vin, 0);
        inode_mark_deleted(&vin);
        inode_write(f, victim, &vin);
        inode_free(f, victim, false);
    }

    rc = dir_add(f, to_parent, &tp_in, to_name, to_len, ino, ftype);
    if (rc < 0) {
        return rc;
    }

    // Re-read the source parent: it is the same inode as the target parent in
    // the common same-directory case, and dir_add may have grown it.
    if (from_parent == to_parent) {
        fp_in = tp_in;
    }
    return dir_remove(f, from_parent, &fp_in, from_name, from_len);
}

int mkdir(const char *path)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    SCOPE_LOCK(f->lock);

    uint32_t parent_ino = 0;
    inode parent;
    const char *name = nullptr;
    size_t name_len = 0;
    int rc = path_split(f, path, &parent_ino, &parent, &name, &name_len);
    if (rc < 0) {
        return rc;
    }
    if (!inode_is_dir(&parent)) {
        return -ENOTDIR;
    }

    uint32_t existing = 0;
    rc = dir_lookup(f, &parent, name, name_len, &existing);
    if (rc < 0) {
        return rc;
    }
    if (existing) {
        return -EEXIST;
    }

    uint32_t ino = 0;
    rc = inode_alloc(f, true, &ino);
    if (rc < 0) {
        return rc;
    }

    inode in;
    inode_init(f, &in, MODE_DIR | 0755);
    in.i_links_count = le16(2);         // the entry in the parent, plus "."
    rc = inode_write(f, ino, &in);
    if (rc < 0) {
        inode_free(f, ino, true);
        return rc;
    }

    // A directory's first block holds "." and ".." spanning the whole block.
    uint64_t phys = 0;
    uint32_t run = 0;
    rc = extent_map_write(f, ino, &in, 0, 1, &phys, &run);
    if (rc < 0) {
        inode_free(f, ino, true);
        return rc;
    }

    scratch buf(f->block_size);
    if (!buf) {
        inode_free(f, ino, true);
        return -ENOMEM;
    }
    memset(buf.data(), 0, f->block_size);

    auto *dot = reinterpret_cast<dir_entry *>(buf.data());
    dot->inode = le32(ino);
    dot->rec_len = le16(12);
    dot->name_len = 1;
    dot->file_type = FT_DIR;
    dot->name[0] = '.';

    auto *dotdot = reinterpret_cast<dir_entry *>(buf.data() + 12);
    dotdot->inode = le32(parent_ino);
    dotdot->rec_len = le16(static_cast<uint16_t>(f->block_size - 12));
    dotdot->name_len = 2;
    dotdot->file_type = FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    rc = f->dev.write(buf.data(), phys, 1);
    if (rc < 0) {
        inode_free(f, ino, true);
        return rc;
    }

    inode_set_size(&in, f->block_size);
    rc = inode_write(f, ino, &in);
    if (rc < 0) {
        inode_free(f, ino, true);
        return rc;
    }

    rc = dir_add(f, parent_ino, &parent, name, name_len, ino, FT_DIR);
    if (rc < 0) {
        inode_free(f, ino, true);
        return rc;
    }

    // ".." in the new directory counts as a link to the parent.
    parent.i_links_count = le16(static_cast<uint16_t>(le16(parent.i_links_count) + 1));
    return inode_write(f, parent_ino, &parent);
}

int rmdir(const char *path)
{
    fs *f = get_fs();
    if (!f->mounted) {
        return -EINVAL;
    }
    SCOPE_LOCK(f->lock);

    uint32_t parent_ino = 0;
    inode parent;
    const char *name = nullptr;
    size_t name_len = 0;
    int rc = path_split(f, path, &parent_ino, &parent, &name, &name_len);
    if (rc < 0) {
        return rc;
    }

    uint32_t ino = 0;
    rc = dir_lookup(f, &parent, name, name_len, &ino);
    if (rc < 0) {
        return rc;
    }
    if (ino == 0) {
        return -ENOENT;
    }
    if (ino == ROOT_INO) {
        return -EBUSY;
    }

    inode in;
    rc = inode_read(f, ino, &in);
    if (rc < 0) {
        return rc;
    }
    if (!inode_is_dir(&in)) {
        return -ENOTDIR;
    }

    bool empty = true;
    rc = dir_iterate(f, &in, [&](const char *n, size_t len, uint32_t, uint8_t) {
        if ((len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.')) {
            return true;
        }
        empty = false;
        return false;
    });
    if (rc < 0) {
        return rc;
    }
    if (!empty) {
        return -ENOTEMPTY;
    }

    rc = dir_remove(f, parent_ino, &parent, name, name_len);
    if (rc < 0) {
        return rc;
    }
    rc = extent_truncate(f, ino, &in, 0);
    if (rc < 0) {
        return rc;
    }
    inode_mark_deleted(&in);
    inode_write(f, ino, &in);
    rc = inode_free(f, ino, true);
    if (rc < 0) {
        return rc;
    }

    if (le16(parent.i_links_count) > 2) {
        parent.i_links_count =
            le16(static_cast<uint16_t>(le16(parent.i_links_count) - 1));
    }
    return inode_write(f, parent_ino, &parent);
}

} // namespace miniext
