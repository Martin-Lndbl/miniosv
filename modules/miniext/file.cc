/*
 * miniext file operations: the public API from miniext.hh.
 *
 * A `file` is just a resolved inode plus its number. There is no descriptor
 * table -- the caller holds the pointer, which is the whole point of talking to
 * the filesystem directly instead of through a VFS.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "internal.hh"

namespace miniext {

struct file {
    uint32_t ino;
    inode in;
};

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

    SCOPE_LOCK(f->lock);

    uint32_t ino = 0;
    inode in;
    int rc = path_resolve(f, path, &ino, &in);
    if (rc < 0) {
        *err = rc;
        return nullptr;
    }
    if (inode_is_dir(&in)) {
        *err = -EISDIR;
        return nullptr;
    }
    if (!inode_is_reg(&in)) {
        *err = -EINVAL;
        return nullptr;
    }

    auto *fp = new file{ino, in};
    return fp;
}

void close(file *f)
{
    delete f;
}

int64_t pread(file *fp, void *buf, size_t len, uint64_t offset)
{
    if (!fp) {
        return -EINVAL;
    }
    fs *f = get_fs();
    SCOPE_LOCK(f->lock);
    return inode_pread(f, &fp->in, buf, len, offset);
}

uint64_t size(file *fp)
{
    return fp ? inode_size(&fp->in) : 0;
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
    *out = inode_size(&in);
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

} // namespace miniext
