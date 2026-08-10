/*
 * Reading an NVMe namespace as one flat file (miniext.hh, namespace raw).
 *
 * The device layer does all the work; this only turns byte offsets into LBAs
 * and copies the partial LBAs at each end through a scratch buffer. Whole LBAs
 * in the middle go straight into the caller's buffer, so a large aligned read
 * -- which is what a model loader issues -- costs no copy beyond the DMA.
 *
 * Reads run at LBA granularity because that is the device's unit, so the
 * "block size" here is the LBA size: no filesystem, and nothing to align to
 * except the hardware.
 */

#include <cerrno>
#include <cstring>

#include "internal.hh"

namespace miniext {
namespace raw {

struct device {
    miniext::device dev;
    uint32_t lba_size = 0;
    uint64_t bytes = 0;
};

device *open(int nvme_id, int *err)
{
    auto set = [err](int e) { if (err) { *err = e; } };

    auto *d = new device();
    int rc = d->dev.open(nvme_id);
    if (rc < 0) {
        delete d;
        set(rc);
        return nullptr;
    }
    // One LBA per "block": the device layer's block abstraction has nothing to
    // model here, so make the two the same size and let it pass reads through.
    rc = d->dev.set_block_size(d->dev.lba_size());
    if (rc < 0) {
        d->dev.close();
        delete d;
        set(rc);
        return nullptr;
    }

    d->lba_size = d->dev.lba_size();
    d->bytes = d->dev.lba_count() * static_cast<uint64_t>(d->lba_size);
    set(0);
    return d;
}

void close(device *d)
{
    if (!d) {
        return;
    }
    d->dev.close();
    delete d;
}

uint64_t size(device *d)
{
    return d ? d->bytes : 0;
}

int64_t pread(device *d, void *buf, size_t len, uint64_t offset)
{
    if (!d || !buf) {
        return -EINVAL;
    }
    if (offset >= d->bytes) {
        return 0;
    }
    if (len > d->bytes - offset) {
        len = d->bytes - offset;
    }
    if (len == 0) {
        return 0;
    }

    const uint32_t lba_size = d->lba_size;
    uint8_t *out = static_cast<uint8_t *>(buf);
    size_t moved = 0;

    while (moved < len) {
        const uint64_t pos = offset + moved;
        const uint64_t lba = pos / lba_size;
        const uint32_t in_lba = static_cast<uint32_t>(pos % lba_size);
        size_t want = len - moved;

        if (in_lba == 0 && want >= lba_size) {
            // Whole LBAs, straight into the caller's buffer.
            const uint32_t count = static_cast<uint32_t>(want / lba_size);
            int rc = d->dev.read(out + moved, lba, count);
            if (rc < 0) {
                return rc;
            }
            moved += static_cast<size_t>(count) * lba_size;
            continue;
        }

        // A partial LBA at one end or the other.
        const size_t to_lba_end = lba_size - in_lba;
        if (want > to_lba_end) {
            want = to_lba_end;
        }
        scratch s(lba_size);
        if (!s) {
            return -ENOMEM;
        }
        int rc = d->dev.read(s.data(), lba, 1);
        if (rc < 0) {
            return rc;
        }
        memcpy(out + moved, s.data() + in_lba, want);
        moved += want;
    }

    return static_cast<int64_t>(moved);
}

} // namespace raw
} // namespace miniext
