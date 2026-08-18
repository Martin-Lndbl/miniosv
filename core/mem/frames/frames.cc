/*
 * The frame allocator: a façade over llfree.
 */

#include <atomic>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/frames.hh>
#include <osv/mmu.hh>
#include <osv/sched.hh>

#include "internal.hh"

namespace mem {
namespace frames {

static_assert(page_size == mmu::page_size, "frames::page_size disagrees with mmu");

namespace {

llfree_t *llf;
uintptr_t base_linear;     // frame 0
size_t frame_count;
size_t total;

/*
* llfree starts with every frame marked allocated and boot.cc then gives back
* whatever it never handed out to others subsystems during boot.
*/
void put_run(uintptr_t start, uintptr_t end)
{
    for_each_block(frame_of(start), frame_of(end), block_max,
                   [](uint64_t frame, unsigned order) {
        // Everything starts allocated and is given back exactly once, so this
        // cannot legitimately fail.
        (void)llfree_put(llf, 0, frame, llflags(order));
    });
}

} // namespace

llfree_t *allocator()
{
    return llf;
}

uint64_t frame_of(uintptr_t linear)
{
    return (linear - base_linear) >> mmu::page_size_shift;
}

uintptr_t linear_of(uint64_t frame)
{
    return base_linear + (frame << mmu::page_size_shift);
}

phys_addr phys_of(uint64_t frame)
{
    return mmu::virt_to_phys(reinterpret_cast<void *>(linear_of(frame)));
}

uint64_t frame_of_phys(phys_addr p)
{
    return frame_of(reinterpret_cast<uintptr_t>(mmu::phys_to_virt(p)));
}

size_t total_frames()
{
    return frame_count;
}

// Until the scheduler runs on every cpu there is no current cpu to ask, and
// llfree indexes its per-core state with whatever it is given.
std::atomic<bool> percpu_ready;

size_t current_core()
{
    if (!percpu_ready.load(std::memory_order_relaxed)) {
        return 0;
    }
    sched::cpu *c = sched::cpu::current();
    if (!c) {
        return 0;
    }
    size_t cores = llfree_cores(llf);
    return c->id < cores ? c->id : c->id % cores;
}

void enable_percpu()
{
    percpu_ready.store(true, std::memory_order_relaxed);
}

bool ready()
{
    return llf != nullptr;
}

void init(size_t cores)
{
    if (llf) {
        return;
    }

    uintptr_t lowest, highest;
    boot_bounds(lowest, highest);
    if (lowest >= highest) {
        return;
    }

    // llfree wants its region aligned to the largest block it serves.
    base_linear = align_down(lowest, static_cast<uintptr_t>(LLFREE_ALIGN));
    frame_count = (highest - base_linear) >> mmu::page_size_shift;

    llfree_meta_size_t sizes = llfree_metadata_size(cores, frame_count);
    llfree_meta_t meta = {
        .local = static_cast<uint8_t *>(boot_alloc(sizes.local, LLFREE_CACHE_SIZE)),
        .trees = static_cast<uint8_t *>(boot_alloc(sizes.trees, LLFREE_CACHE_SIZE)),
        .lower = static_cast<uint8_t *>(boot_alloc(sizes.lower, LLFREE_CACHE_SIZE)),
    };
    llfree_t *self = static_cast<llfree_t *>(boot_alloc(sizes.llfree, LLFREE_CACHE_SIZE));
    if (!self || !meta.local || !meta.trees || !meta.lower) {
        abort("frames: no memory for the frame allocator's own metadata\n");
    }

    llfree_result_t r = llfree_init(self, cores, frame_count, LLFREE_INIT_ALLOC, meta);
    if (!llfree_is_ok(r)) {
        abort("frames: llfree_init failed\n");
    }
    llf = self;

    boot_for_each_free([](uintptr_t start, uintptr_t end) {
        put_run(start, end);
        total += end - start;
    });

    // alloc() reports failure as physical address 0, so make sure no frame can
    // ever carry that address. 
    // For now, this never happened but guard just in case. 
    if (phys_of(0) == no_memory) {
        llfree_result_t claim = llfree_get_at(llf, 0, 0, llflags(0));
        if (llfree_is_ok(claim)) {
            total -= page_size;
        }
    }

    pressure_init(total);
}

phys_addr alloc(size_t bytes, size_t align)
{
    if (!bytes) {
        return no_memory;
    }
    if (align < page_size) {
        align = page_size;
    }
    size_t need = frames_for(bytes);

    if (!llf) {
        void *p = boot_alloc(need << mmu::page_size_shift, align);
        return p ? from_linear(p) : no_memory;
    }

    // One block, which llfree hands out aligned to its own size. The common
    // case, and the only one that does not search.
    unsigned order = order_of(need);
    if (order <= block_max && (page_size << order) >= align) {
        llfree_result_t r = llfree_get(llf, current_core(), llflags(order));
        if (!llfree_is_ok(r)) {
            return no_memory;
        }
        check_pressure();
        return phys_of(r.frame);
    }

    // Try to claim multiple contiguous blocks.
    uint64_t frame = claim_run(need, align);
    if (frame == no_frame) {
        return no_memory;
    }
    check_pressure();
    return phys_of(frame);
}

void free(phys_addr addr, size_t bytes)
{
    if (!addr || !bytes) {
        return;
    }
    if (!llf) {
        boot_free_page(to_linear(addr));
        return;
    }
    uint64_t first = frame_of_phys(addr);
    release_run(first, first + frames_for(bytes));
}

void *to_linear(phys_addr p)
{
    return mmu::phys_to_virt(p);
}

phys_addr from_linear(void *addr)
{
    return mmu::virt_to_phys(addr);
}

size_t total_bytes()
{
    return llf ? total : boot_total();
}

// From llfree's own counters
size_t free_bytes()
{
    if (!llf) {
        return boot_total();
    }
    return llfree_free_frames(llf) << mmu::page_size_shift;
}

} // namespace frames
} // namespace mem
