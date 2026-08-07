/*
 * Application arguments, carried in a reserved block on the boot disk.
 *
 * miniOSv boots via UEFI from removable media, so EFI LoadOptions is empty and
 * no command line reaches the kernel that way. The obvious alternative,
 * QEMU's fw_cfg, is a QEMU interface: AWS Nitro, GCP and Azure do not expose
 * it, and this kernel is meant to boot on all three.
 *
 * What every one of them does have in common is the boot disk itself. The GPT
 * image leaves LBA 34..2047 unused between the primary partition table and the
 * ESP (scripts/mkuefi.sh), so one sector in that gap carries a magic-tagged,
 * NUL-terminated argument string. Nothing but this code interprets it, so it
 * works the same under QEMU and on any cloud, on both architectures.
 *
 * This is the same trick OSv's scripts/imgedit.py used on the old MBR images,
 * moved to a GPT-safe offset. scripts/setargs.py rewrites the block in place,
 * so changing arguments does not mean rebuilding the kernel.
 */

#ifndef OSV_BOOTARGS_HH
#define OSV_BOOTARGS_HH

#include <string>
#include <vector>

namespace osv {

// Sector holding the block, and its on-disk layout. LBA 34 is the first sector
// after the GPT partition entry array on a 512-byte-sector disk.
static const unsigned bootargs_lba = 34;
static const char bootargs_magic[8] = {'M', 'I', 'N', 'I', 'A', 'R', 'G', 'S'};
static const unsigned bootargs_max = 448;

struct bootargs_block {
    char magic[8];              // "MINIARGS"
    uint32_t length;            // bytes of args, excluding the NUL
    uint32_t reserved;
    char args[bootargs_max];    // NUL-terminated
};

// Read the block off the boot disk. Returns an empty string when the disk has
// no block, the magic does not match, or no storage driver is available --
// callers treat that as "no arguments", not as an error.
std::string bootargs();

// Split into whitespace-separated words, honouring single and double quotes so
// an argument can contain spaces (e.g. -c 'SELECT 42').
std::vector<std::string> bootargs_split(const std::string &line);

} // namespace osv

#endif /* OSV_BOOTARGS_HH */
