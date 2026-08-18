/*
 * Memory pressure notification.
 *
 * Subsystems that can relinquish memory register a callback here and are notified
 * when free memory falls below a threshold. 
 * 
 * Callbacks runs on the allocation path, so must not block or allocate.
 */

#include <atomic>

#include <osv/kernel_config.h>
#include <osv/mem/frames.hh>

#include "internal.hh"

namespace mem {
namespace frames {

namespace {

std::atomic<pressure_watcher *> watchers;
size_t threshold;
std::atomic<bool> in_callback;

} // namespace

void pressure_init(size_t total_bytes)
{
    threshold = total_bytes / 100 * CONF_memory_pressure_percent;
}

void watch_pressure(pressure_watcher &w, pressure_fn cb)
{
    w.fn = cb;
    w.next = watchers.load(std::memory_order_relaxed);
    while (!watchers.compare_exchange_weak(w.next, &w, std::memory_order_release,
                                           std::memory_order_relaxed)) {
    }
}

void check_pressure()
{
    pressure_watcher *head = watchers.load(std::memory_order_acquire);
    if (!head || !threshold || free_bytes() >= threshold) {
        return;
    }
    // One responder at a time: the callbacks free memory, and re-entering from
    // inside one would recurse.
    bool expected = false;
    if (!in_callback.compare_exchange_strong(expected, true)) {
        return;
    }
    for (pressure_watcher *w = head; w; w = w->next) {
        w->fn();
    }
    in_callback.store(false);
}

} // namespace frames
} // namespace mem
