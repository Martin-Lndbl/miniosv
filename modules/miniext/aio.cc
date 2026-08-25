/*
 * Asynchronous IO capabilities for miniext
*/

#include <cerrno>
#include <cstring>

#include <osv/mem/store.hh>

#include "internal.hh"

namespace miniext {

struct aio {
    io_group g;
    open_inode *oi = nullptr;
    bool exclusive = false;

    // Set once the transfers land, for a write that added extents or moved the
    // end of the file: metadata after data, which is the only ordering there is.
    uint32_t ino = 0;
    bool write_inode = false;

    int64_t moved = 0;
    int err = 0;
};

namespace {

// Whole blocks go to the device as they are; a hole and the ragged end of a
// file are handled here, since neither is a transfer.
int64_t read_start(fs *f, const inode *in, aio *a, void *buf, size_t len,
                   uint64_t offset)
{
    const uint64_t fsize = inode_size(in);
    if (offset >= fsize) {
        return 0;
    }
    if (len > fsize - offset) {
        len = fsize - offset;
    }

    auto *out = static_cast<uint8_t *>(buf);
    size_t moved = 0;
    while (moved < len) {
        const uint64_t pos = offset + moved;
        const uint32_t fblock = static_cast<uint32_t>(pos / f->block_size);
        const uint32_t in_block = pos % f->block_size;

        uint64_t phys = 0;
        uint32_t run = 1;
        int rc = extent_lookup(f, in, fblock, &phys, &run);
        if (rc < 0) {
            return moved ? static_cast<int64_t>(moved) : rc;
        }

        size_t want = len - moved;
        if (in_block != 0 || want < f->block_size) {
            const size_t to_block_end = f->block_size - in_block;
            if (want > to_block_end) {
                want = to_block_end;
            }
        }

        if (phys == 0) {
            size_t span = static_cast<size_t>(run) * f->block_size - in_block;
            if (want > span) {
                want = span;
            }
            memset(out + moved, 0, want);
            moved += want;
            continue;
        }

        if (in_block == 0 && want >= f->block_size) {
            uint32_t nblocks = static_cast<uint32_t>(want / f->block_size);
            if (nblocks > run) {
                nblocks = run;
            }
            rc = f->dev.read_async(out + moved, phys, nblocks, a->g);
            if (rc < 0) {
                return moved ? static_cast<int64_t>(moved) : rc;
            }
            moved += static_cast<size_t>(nblocks) * f->block_size;
            continue;
        }

        scratch tmp(f->block_size);
        if (!tmp) {
            return moved ? static_cast<int64_t>(moved) : -ENOMEM;
        }
        rc = f->dev.read(tmp.data(), phys, 1);
        if (rc < 0) {
            return moved ? static_cast<int64_t>(moved) : rc;
        }
        memcpy(out + moved, tmp.data() + in_block, want);
        moved += want;
    }
    return static_cast<int64_t>(moved);
}

int64_t write_start(fs *f, uint32_t ino, inode *in, aio *a, const void *buf,
                    size_t len, uint64_t offset)
{
    const auto *src = static_cast<const uint8_t *>(buf);
    size_t moved = 0;

    while (moved < len) {
        const uint64_t pos = offset + moved;
        const uint32_t fblock = static_cast<uint32_t>(pos / f->block_size);
        const uint32_t in_block = pos % f->block_size;
        const size_t remaining = len - moved;

        uint32_t want = static_cast<uint32_t>((in_block + remaining +
                                               f->block_size - 1) / f->block_size);
        if (want == 0) {
            want = 1;
        }

        uint64_t phys = 0;
        uint32_t run = 0;
        const bool was_hole = ([&] {
            uint64_t p = 0;
            uint32_t r = 0;
            return extent_lookup(f, in, fblock, &p, &r) == 0 && p == 0;
        })();

        int rc = extent_map_write(f, ino, in, fblock, want, &phys, &run);
        if (rc < 0) {
            return moved ? static_cast<int64_t>(moved) : rc;
        }

        if (in_block == 0 && remaining >= f->block_size) {
            uint32_t nblocks = static_cast<uint32_t>(remaining / f->block_size);
            if (nblocks > run) {
                nblocks = run;
            }
            rc = f->dev.write_async(src + moved, phys, nblocks, a->g);
            if (rc < 0) {
                return moved ? static_cast<int64_t>(moved) : rc;
            }
            moved += static_cast<size_t>(nblocks) * f->block_size;
            continue;
        }

        // Partial block: read, change, write, all of it before this returns.
        // A block that was a hole a moment ago holds whatever its last owner
        // left, so it starts from zeros rather than from what is on the media.
        size_t want_bytes = remaining;
        const size_t to_block_end = f->block_size - in_block;
        if (want_bytes > to_block_end) {
            want_bytes = to_block_end;
        }
        scratch tmp(f->block_size);
        if (!tmp) {
            return moved ? static_cast<int64_t>(moved) : -ENOMEM;
        }
        if (was_hole) {
            memset(tmp.data(), 0, f->block_size);
        } else {
            rc = f->dev.read(tmp.data(), phys, 1);
            if (rc < 0) {
                return moved ? static_cast<int64_t>(moved) : rc;
            }
        }
        memcpy(tmp.data() + in_block, src + moved, want_bytes);
        rc = f->dev.write(tmp.data(), phys, 1);
        if (rc < 0) {
            return moved ? static_cast<int64_t>(moved) : rc;
        }
        moved += want_bytes;
    }
    return static_cast<int64_t>(moved);
}

} // namespace

void oi_io_begin(open_inode *oi)
{
    WITH_LOCK(oi->io_lock) {
        oi->inflight++;
    }
}

void oi_io_end(open_inode *oi)
{
    WITH_LOCK(oi->io_lock) {
        if (--oi->inflight == 0) {
            oi->io_idle.wake_all();
        }
    }
}

void oi_io_drain(open_inode *oi)
{
    WITH_LOCK(oi->io_lock) {
        while (oi->inflight) {
            oi->io_idle.wait(oi->io_lock);
        }
    }
}

aio *aread(file *fp, void *buf, size_t len, uint64_t offset)
{
    if (!fp) {
        return nullptr;
    }
    auto *a = new (std::nothrow) aio();
    if (!a) {
        return nullptr;
    }
    a->oi = fp->oi;

    int64_t rc;
    WITH_LOCK(fp->oi->lock.for_read()) {
        rc = read_start(get_fs(), &fp->oi->in, a, buf, len, offset);
        // Counted before the lock goes, so a writer that takes it next sees
        // this transfer and waits for it.
        oi_io_begin(fp->oi);
    }
    if (rc < 0) {
        a->err = static_cast<int>(rc);
    } else {
        a->moved = rc;
    }
    // Whatever was placed is in flight now, so the hold that kept the group
    // from settling half way through the submissions can go.
    a->g.drop();
    return a;
}

aio *awrite(file *fp, const void *buf, size_t len, uint64_t offset)
{
    if (!fp) {
        return nullptr;
    }
    if (!(fp->flags & O_WR)) {
        return nullptr;
    }
    auto *a = new (std::nothrow) aio();
    if (!a) {
        return nullptr;
    }
    a->oi = fp->oi;
    a->exclusive = true;
    a->ino = fp->oi->ino;

    // Held all the way to await(): a write excludes everything else on this
    // file, and the lock is recursive for a writer, so a caller batching
    // several of these is not blocking itself.
    fp->oi->lock.wlock();
    oi_io_drain(fp->oi);
    fs *f = get_fs();
    int64_t rc = len ? write_start(f, fp->oi->ino, &fp->oi->in, a, buf, len, offset) : 0;
    if (rc < 0) {
        a->err = static_cast<int>(rc);
    } else {
        a->moved = rc;
        if (offset + rc > inode_size(&fp->oi->in)) {
            inode_set_size(&fp->oi->in, offset + rc);
        }
        a->write_inode = true;
    }
    a->g.drop();
    return a;
}

bool adone(aio *a)
{
    return a && a->g.settled();
}

int64_t await(aio *a)
{
    if (!a) {
        return -EINVAL;
    }
    a->g.waiter.reset(*sched::thread::current());
    sched::thread::wait_until([a] { return a->g.settled(); });
    a->g.waiter.clear();

    int err = a->err ? a->err : a->g.error.load(std::memory_order_relaxed);
    if (!err && a->write_inode) {
        // The extents and the size only become true once the data they
        // describe is on the media.
        int rc = inode_write(get_fs(), a->ino, &a->oi->in);
        if (rc < 0) {
            err = rc;
        }
    }
    if (a->exclusive) {
        a->oi->lock.wunlock();
    } else {
        oi_io_end(a->oi);
    }
    int64_t moved = a->moved;
    delete a;
    return err ? err : moved;
}

/* The page cache's backend. */

namespace {

class file_store : public mem::store {
public:
    file_store(file *f, uint64_t bytes, size_t block) : _f(f), _bytes(bytes), _block(block) {}

    uint64_t size() override { return _bytes; }
    size_t granularity() override { return _block; }

    mem::io *read(void *buf, uint64_t offset, size_t bytes) override
    {
        return reinterpret_cast<mem::io *>(aread(_f, buf, bytes, offset));
    }

    mem::io *write(const void *buf, uint64_t offset, size_t bytes) override
    {
        return reinterpret_cast<mem::io *>(awrite(_f, buf, bytes, offset));
    }

    bool done(mem::io *req) override
    {
        return adone(reinterpret_cast<aio *>(req));
    }

    int64_t wait(mem::io *req) override
    {
        return await(reinterpret_cast<aio *>(req));
    }

private:
    file *_f;
    uint64_t _bytes;
    size_t _block;
};

} // namespace

mem::store *store_open(file *f)
{
    fs_info info_out;
    if (!f || info(&info_out) < 0) {
        return nullptr;
    }
    return new (std::nothrow) file_store(f, size(f), info_out.block_size);
}

void store_close(mem::store *s)
{
    delete s;
}

} // namespace miniext
