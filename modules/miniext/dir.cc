/*
 * miniext directories and path resolution.
 *
 * Directories are linear: each data block holds a chain of dir_entry records
 * whose rec_len fields step from one to the next, the last one running to the
 * end of the block. An entry never straddles a block boundary. inode == 0 marks
 * a free slot, which a lookup skips.
 *
 * scripts/mkdata.sh disables dir_index precisely so this stays true -- with
 * HTree directories the same linear scan would still read correctly, but
 * inserting an entry without maintaining the hash tree would corrupt them.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "internal.hh"

namespace miniext {

int dir_iterate(fs *f, const inode *dir,
                const std::function<bool(const char *, size_t, uint32_t, uint8_t)> &cb)
{
    if (!inode_is_dir(dir)) {
        return -ENOTDIR;
    }

    const uint64_t dsize = inode_size(dir);
    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }

    for (uint64_t off = 0; off < dsize; off += f->block_size) {
        int64_t got = inode_pread(f, dir, buf.data(), f->block_size, off);
        if (got < 0) {
            return static_cast<int>(got);
        }
        if (got == 0) {
            break;
        }

        uint32_t pos = 0;
        while (pos + DIR_ENTRY_HEADER <= static_cast<uint32_t>(got)) {
            const dir_entry *de =
                reinterpret_cast<const dir_entry *>(buf.data() + pos);
            const uint16_t rec_len = le16(de->rec_len);

            // A zero or misaligned rec_len would loop forever or walk off the
            // block; treat it as corruption rather than trusting it.
            if (rec_len < DIR_ENTRY_HEADER || (rec_len & 3) != 0 ||
                pos + rec_len > static_cast<uint32_t>(got)) {
                printf("miniext: corrupt dirent at block offset %u (rec_len %u)\n",
                       pos, rec_len);
                return -EIO;
            }

            if (le32(de->inode) != 0 && de->name_len != 0 &&
                DIR_ENTRY_HEADER + de->name_len <= rec_len) {
                if (!cb(de->name, de->name_len, le32(de->inode), de->file_type)) {
                    return 0;   // callback asked to stop
                }
            }
            pos += rec_len;
        }
    }
    return 0;
}

int dir_lookup(fs *f, const inode *dir, const char *name, size_t name_len,
               uint32_t *out_ino)
{
    *out_ino = 0;
    if (name_len == 0 || name_len > 255) {
        return -EINVAL;
    }

    uint32_t found = 0;
    int rc = dir_iterate(f, dir,
        [&](const char *ename, size_t elen, uint32_t ino, uint8_t) {
            if (elen == name_len && memcmp(ename, name, name_len) == 0) {
                found = ino;
                return false;
            }
            return true;
        });
    if (rc < 0) {
        return rc;
    }
    *out_ino = found;
    return 0;
}

// Resolve an absolute path that starts with the mount point. Returns -ENOENT
// for a missing component and -ENOTDIR when a non-final component is not a
// directory.
int path_resolve(fs *f, const char *path, uint32_t *out_ino, inode *out)
{
    if (!f->mounted) {
        return -EINVAL;
    }
    if (!path || path[0] != '/') {
        return -EINVAL;
    }

    // Strip the mount point prefix.
    const size_t mplen = f->mount_point.size();
    const char *rel = path;
    if (f->mount_point != "/") {
        if (strncmp(path, f->mount_point.c_str(), mplen) != 0) {
            return -ENOENT;
        }
        if (path[mplen] != '\0' && path[mplen] != '/') {
            return -ENOENT;     // e.g. "/dbx" against a "/db" mount
        }
        rel = path + mplen;
    }
    if (*rel == '\0') {
        rel = "/";
    }

    uint32_t ino = ROOT_INO;
    inode cur;
    int rc = inode_read(f, ino, &cur);
    if (rc < 0) {
        return rc;
    }

    const char *p = rel;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        const size_t len = p - start;

        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (!inode_is_dir(&cur)) {
            return -ENOTDIR;
        }

        uint32_t next = 0;
        rc = dir_lookup(f, &cur, start, len, &next);
        if (rc < 0) {
            return rc;
        }
        if (next == 0) {
            return -ENOENT;
        }

        rc = inode_read(f, next, &cur);
        if (rc < 0) {
            return rc;
        }
        ino = next;
    }

    *out_ino = ino;
    *out = cur;
    return 0;
}

} // namespace miniext
