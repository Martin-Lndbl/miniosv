/*
 * The sub-page allocator.
 *
 * Objects are packed into pages that are reclaimed whole when the last object
 * in them dies, so fragmentation is handled by the mmu rather than by
 * coalescing. Allocations too big for that get a reservation and frames of
 * their own, which is what is here so far.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_HEAP_HH
#define OSV_MEM_HEAP_HH

#include <osv/mem/types.hh>

namespace mem {
namespace heap {

// From here up, an allocation is worth a reservation of its own: the frames
// behind it are a whole number of huge pages and the mapping is one leaf each.
constexpr size_t large_min = 2ul << 20;

// Reserve, allocate and map `bytes`, or null if any of the three fails.
// The result is aligned to large_min, so it satisfies any alignment up to it.
void *large_alloc(size_t bytes);

// Give back what large_alloc returned. The pointer must be one of its results.
void large_free(void *p);

// What large_alloc was asked for, which is less than what it mapped.
size_t large_size(void *p);

// True if this address is one large_alloc handed out.
bool is_large(void *p);

}
}

#endif /* OSV_MEM_HEAP_HH */
