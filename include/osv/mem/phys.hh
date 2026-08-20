/*
 * Physical memory the kernel did not allocate.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_PHYS_HH
#define OSV_MEM_PHYS_HH

#include <osv/mem/types.hh>

namespace mem {

/*
 * A pointer to [pa, pa+bytes). Range is rounded out to whole page.
 * Mapping twice is allowed if the frames correspond (drivers do this).

 * Never unmapped: everything mapped with this function shares the lifetime of the kernel.
 */
void *map_phys(frames::phys_addr pa, size_t bytes, mattr ma = mattr::normal);

}

#endif /* OSV_MEM_PHYS_HH */
