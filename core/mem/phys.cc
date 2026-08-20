/*
 * Handle mappings not managed by the kernel.
 * Mostly (only?) for the drivers
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>

#include "linear.hh"

namespace mem {

void *map_phys(frames::phys_addr pa, size_t bytes, mattr ma)
{
    auto start = align_down(pa, mapping::page_size);
    auto end = align_up(pa + bytes, mapping::page_size);
    auto virt = reinterpret_cast<uintptr_t>(frames::to_linear(start));

    if (!mapping::attach_missing({virt, virt + (end - start)}, start, perm_rw,
                                 mapping::huge_page_size, ma)) {
        abort("map_phys: %p is already mapped to other physical memory\n",
              reinterpret_cast<void *>(pa));
    }
    return frames::to_linear(pa);
}

}
