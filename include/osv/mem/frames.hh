/*
 * Physical frame allocation.
 * Backed by LLFree[ATC'23] (external/llfree).
 */

#ifndef OSV_MEM_FRAMES_HH
#define OSV_MEM_FRAMES_HH

#include <cstddef>
#include <cstdint>

#include <osv/mem/types.hh>

namespace mem {
namespace frames {

constexpr size_t page_size = 4096;
constexpr size_t max_block_bytes = page_size << 9;   // 2 MiB

/*
* Allocates a physical region of at least `bytes` bytes, aligned to `align`. 
* Returns the physical address of the first byte, or no_memory if not enough free space.
* 
* bytes is rounded up: to a whole block when it fits in one, to whole frames
* otherwise. 
* free() repeats that calculation, so it must be given the size that
* was asked for.
*/
phys_addr alloc(size_t bytes = page_size, size_t align = page_size);
void free(phys_addr addr, size_t bytes = page_size);

// Every frame is reachable through the kernel's linear map.
void *to_linear(phys_addr p);
phys_addr from_linear(void *addr);

size_t free_bytes();
size_t total_bytes();

// Called when free memory falls below conf_memory_pressure_percent of the
// total. Callbacks run on the freeing path, so they must not block.
using pressure_fn = void (*)();

struct pressure_watcher {
    pressure_fn fn = nullptr;
    pressure_watcher *next = nullptr;
};

void watch_pressure(pressure_watcher &w, pressure_fn cb);
void check_pressure();

// Boot. add_region() collects memory before there is an allocator to put it in;
// init() builds llfree and hands it everything still unused.
void add_region(void *base, size_t bytes);
void init(size_t cores);
// Call once the scheduler runs everywhere: until then allocation uses core 0.
void enable_percpu();
bool ready();

// Pre-init allocation, from the regions collected by add_region().
void *boot_alloc(size_t bytes, size_t align, size_t offset = 0);
void *boot_alloc_page();
void boot_free_page(void *addr);

} // namespace frames
} // namespace mem

#endif
