#!/usr/bin/env bash
#
# Build an ext4 data disk for the application to mount with modules/miniext.
# The target is either a raw image file or a real block device:
#
#   # image file, attached as a second emulated NVMe device
#   scripts/mkdata.sh build/data.img [staging-dir] [size]
#   scripts/run.py --emulated-nvme build/data.img
#
#   # real NVMe namespace, handed to the guest via vfio-pci passthrough
#   sudo scripts/mkdata.sh /dev/nvme1n1 [staging-dir]
#   scripts/run.py --pass-pci 0000:01:00.0
#
# Formatting a block device DESTROYS it, so that path asks first. Set
# MKDATA_ASSUME_YES=1 to skip the prompt (required when stdin is not a tty).
# The [size] argument applies to image files only; a device's size is its own.
#
# Either way the result is a bare filesystem: no partition table, no GPT.
# Filesystem block 0 is LBA 0, so miniext maps block n to LBA
# n * (block_size / lba_size). For passthrough, format the namespace itself
# (/dev/nvme1n1), not a partition on it.
#
# miniext implements a deliberately narrow subset of ext4, so the image is
# created with everything outside that subset switched off. Each -O ^feature
# below is a feature we chose not to write code for:
#
#   ^has_journal    No JBD2. Nothing has to write a journal on every metadata
#                   update. The cost is that an unclean shutdown leaves the
#                   filesystem needing a host-side 'fsck.ext4 -f -y'.
#   ^dir_index      No HTree directories. HTree dirs can be *read* by a linear
#                   scan, but inserting into one without maintaining the hash
#                   tree corrupts it -- and we create files (WAL, temp).
#   ^metadata_csum  No crc32c on metadata blocks, so writes do not have to
#                   recompute one to stay fsck-clean.
#   ^uninit_bg      Every bitmap is really initialised on disk. With this on,
#                   BLOCK_UNINIT/INODE_UNINIT mean a bitmap block holds garbage
#                   rather than zeros.
#   ^64bit          32-byte group descriptors and 32-bit block numbers.
#
# What survives is what miniext does implement: extent-mapped inodes, filetype
# in dirents, plus flex_bg/sparse_super/large_file/huge_file/dir_nlink/
# extra_isize/ext_attr/resize_inode, which are either allocation policy or
# read-only-compat flags that change nothing we parse.
#
# mkfs.ext4 -d populates the target straight from a directory: no loopback
# mount, no root (for an image file).
#
set -euo pipefail

target="${1:?usage: mkdata.sh <out.img|/dev/nvmeXnY> [staging-dir] [size]}"
staging="${2:-}"
size="${3:-4G}"

if ! command -v mkfs.ext4 >/dev/null 2>&1; then
    echo "mkdata.sh: needs mkfs.ext4 from e2fsprogs." >&2
    echo "           it is in the nix dev shell ('nix develop'); otherwise" >&2
    echo "           install e2fsprogs." >&2
    exit 1
fi

features='^has_journal,^dir_index,^metadata_csum,^uninit_bg,^64bit'

if [ -b "$target" ]; then
    # --- real block device -------------------------------------------------
    if [ -n "${3:-}" ]; then
        echo "mkdata.sh: [size] does not apply to a block device ($target)." >&2
        exit 1
    fi

    # blockdev needs read permission, which a non-root run does not have; fall
    # back to lsblk rather than printing a bogus 0.
    bytes="$(blockdev --getsize64 "$target" 2>/dev/null || echo 0)"
    if [ "$bytes" -gt 0 ]; then
        human="$((bytes / 1024 / 1024)) MiB"
    else
        human="$(lsblk -ndo SIZE "$target" 2>/dev/null || echo 'unknown size')"
    fi
    model="$(lsblk -ndo MODEL "$target" 2>/dev/null || true)"

    echo "mkdata.sh: target is a BLOCK DEVICE"
    echo "           $target  ${model:-unknown model}  $human"

    # Refuse a device with anything mounted on it or on one of its partitions --
    # that is never what is meant, and on a dev box it is typically the host's
    # own boot disk.
    mounts="$(lsblk -no MOUNTPOINTS "$target" 2>/dev/null | grep . || true)"
    if [ -n "$mounts" ]; then
        echo "mkdata.sh: $target has mounted filesystems. Refusing." >&2
        echo "$mounts" | sed 's/^/           mounted: /' >&2
        exit 1
    fi

    if [ "${MKDATA_ASSUME_YES:-0}" != "1" ]; then
        if [ ! -t 0 ]; then
            echo "mkdata.sh: refusing to format $target without confirmation." >&2
            echo "           re-run on a terminal, or set MKDATA_ASSUME_YES=1." >&2
            exit 1
        fi
        printf 'mkdata.sh: this ERASES all data on %s. Type the device path to confirm: ' "$target"
        read -r reply
        if [ "$reply" != "$target" ]; then
            echo "mkdata.sh: aborted." >&2
            exit 1
        fi
    fi
    what="$target ($((bytes / 1024 / 1024)) MiB)"
else
    # --- image file --------------------------------------------------------
    mkdir -p "$(dirname "$target")"
    rm -f "$target"
    truncate -s "$size" "$target"
    what="$target ($size)"
fi

if [ -n "$staging" ]; then
    mkfs.ext4 -q -b 4096 -I 256 -O "$features" -d "$staging" -F "$target"
    echo "mkdata.sh: wrote $what, ext4, populated from $staging"
else
    mkfs.ext4 -q -b 4096 -I 256 -O "$features" -F "$target"
    echo "mkdata.sh: wrote $what, ext4, empty"
fi

# The same check that gates every miniext write test: the filesystem must be
# clean before the guest ever touches it, so a later failure is unambiguously
# ours.
fsck.ext4 -f -n "$target" >/dev/null
echo "mkdata.sh: fsck clean"
