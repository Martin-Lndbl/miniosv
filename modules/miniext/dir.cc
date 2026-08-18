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
#include <string>

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

namespace {
// A record occupies its 8-byte header plus the name, rounded up to 4 bytes.
inline uint16_t dirent_need(size_t name_len)
{
    return static_cast<uint16_t>((DIR_ENTRY_HEADER + name_len + 3) & ~3u);
}
} // namespace

// Link `ino` into `dir` under `name`. A live record only needs room for its own
// name, so the rest of its rec_len is slack that can be split off; failing
// that, a free record is taken whole, and failing that the directory grows by
// one block.
int dir_add(fs *f, uint32_t dir_ino, inode *dir, const char *name,
            size_t name_len, uint32_t ino, uint8_t file_type)
{
    if (name_len == 0 || name_len > 255) {
        return -EINVAL;
    }
    const uint16_t need = dirent_need(name_len);

    // Refuse a duplicate rather than leaving two records for one name.
    uint32_t existing = 0;
    int rc = dir_lookup(f, dir, name, name_len, &existing);
    if (rc < 0) {
        return rc;
    }
    if (existing) {
        return -EEXIST;
    }

    const uint64_t dsize = inode_size(dir);
    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }

    for (uint64_t off = 0; off < dsize; off += f->block_size) {
        uint64_t phys = 0;
        uint32_t run = 0;
        rc = extent_lookup(f, dir, static_cast<uint32_t>(off / f->block_size),
                           &phys, &run);
        if (rc < 0) {
            return rc;
        }
        if (phys == 0) {
            continue;                   // sparse directory block
        }
        rc = f->dev.read(buf.data(), phys, 1);
        if (rc < 0) {
            return rc;
        }

        uint32_t pos = 0;
        while (pos + DIR_ENTRY_HEADER <= f->block_size) {
            auto *de = reinterpret_cast<dir_entry *>(buf.data() + pos);
            const uint16_t rec_len = le16(de->rec_len);
            if (rec_len < DIR_ENTRY_HEADER || pos + rec_len > f->block_size) {
                return -EIO;
            }

            const uint16_t used = le32(de->inode) ? dirent_need(de->name_len) : 0;
            if (rec_len - used >= need) {
                dir_entry *slot;
                if (used == 0) {
                    slot = de;
                } else {
                    de->rec_len = le16(used);
                    slot = reinterpret_cast<dir_entry *>(buf.data() + pos + used);
                    slot->rec_len = le16(static_cast<uint16_t>(rec_len - used));
                }
                slot->inode = le32(ino);
                slot->name_len = static_cast<uint8_t>(name_len);
                slot->file_type = file_type;
                memcpy(slot->name, name, name_len);
                return f->dev.write(buf.data(), phys, 1);
            }
            pos += rec_len;
        }
    }

    // Nowhere to put it: append a block holding one record that spans it.
    memset(buf.data(), 0, f->block_size);
    auto *de = reinterpret_cast<dir_entry *>(buf.data());
    de->inode = le32(ino);
    de->rec_len = le16(static_cast<uint16_t>(f->block_size));
    de->name_len = static_cast<uint8_t>(name_len);
    de->file_type = file_type;
    memcpy(de->name, name, name_len);

    const uint32_t fblock = static_cast<uint32_t>(dsize / f->block_size);
    uint64_t phys = 0;
    uint32_t run = 0;
    rc = extent_map_write(f, dir_ino, dir, fblock, 1, &phys, &run);
    if (rc < 0) {
        return rc;
    }
    rc = f->dev.write(buf.data(), phys, 1);
    if (rc < 0) {
        return rc;
    }

    inode_set_size(dir, dsize + f->block_size);
    return inode_write(f, dir_ino, dir);
}

// Remove `name` by folding its record into the preceding one. The first record
// in a block has nothing to fold into, so it is blanked instead.
int dir_remove(fs *f, uint32_t dir_ino, inode *dir, const char *name,
               size_t name_len)
{
    (void)dir_ino;
    const uint64_t dsize = inode_size(dir);
    scratch buf(f->block_size);
    if (!buf) {
        return -ENOMEM;
    }

    for (uint64_t off = 0; off < dsize; off += f->block_size) {
        uint64_t phys = 0;
        uint32_t run = 0;
        int rc = extent_lookup(f, dir, static_cast<uint32_t>(off / f->block_size),
                               &phys, &run);
        if (rc < 0) {
            return rc;
        }
        if (phys == 0) {
            continue;
        }
        rc = f->dev.read(buf.data(), phys, 1);
        if (rc < 0) {
            return rc;
        }

        uint32_t pos = 0;
        uint32_t prev = 0;
        bool have_prev = false;
        while (pos + DIR_ENTRY_HEADER <= f->block_size) {
            auto *de = reinterpret_cast<dir_entry *>(buf.data() + pos);
            const uint16_t rec_len = le16(de->rec_len);
            if (rec_len < DIR_ENTRY_HEADER || pos + rec_len > f->block_size) {
                return -EIO;
            }

            if (le32(de->inode) && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                if (have_prev) {
                    auto *pd = reinterpret_cast<dir_entry *>(buf.data() + prev);
                    pd->rec_len = le16(static_cast<uint16_t>(
                        le16(pd->rec_len) + rec_len));
                } else {
                    de->inode = le32(0);
                    de->name_len = 0;
                    de->file_type = FT_UNKNOWN;
                }
                return f->dev.write(buf.data(), phys, 1);
            }
            prev = pos;
            have_prev = true;
            pos += rec_len;
        }
    }
    return -ENOENT;
}

// Split an absolute path into its parent directory and final component.
int path_split(fs *f, const char *path, uint32_t *parent_ino, inode *parent,
               const char **name, size_t *name_len)
{
    const char *last = strrchr(path, '/');
    if (!last || last[1] == '\0') {
        return -EINVAL;
    }
    *name = last + 1;
    *name_len = strlen(last + 1);
    if (*name_len > 255) {
        return -ENAMETOOLONG;
    }

    std::string dir_path(path, last == path ? 1 : static_cast<size_t>(last - path));
    return path_resolve(f, dir_path.c_str(), parent_ino, parent);
}

} // namespace miniext
