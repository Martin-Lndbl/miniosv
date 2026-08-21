#ifndef BYPASS_TIME_H
#define BYPASS_TIME_H

#include <cstdint>
#include <osv/clock.hh>
#include <processor.hh>

static constexpr uint64_t NS_PER_S = 1e9;
static constexpr uint64_t NS_PER_US = 1e3;
static constexpr uint64_t US_PER_S = 1e6;

inline uint64_t rte_get_timer_cycles(void) { return processor::ticks(); }

inline uint64_t rte_get_timer_hz(void) {
  auto nanos = clock::get()->processor_to_nano(NS_PER_S);
  return static_cast<uint64_t>(NS_PER_S * NS_PER_S) / nanos;
}

inline void rte_delay_us_block(unsigned int us) {
  const auto end = rte_get_timer_cycles() + us * rte_get_timer_hz() / US_PER_S;
  while (rte_get_timer_cycles() < end)
#ifdef __x86_64__
    ;
#endif
#ifdef __aarch64__
  __asm __volatile("isb sy");
#endif
}

inline void rte_delay_us_sleep(unsigned int us) { rte_delay_us_block(us); }

// Timer/callout API stubs. The DPDK-vendored ENA driver references
// rte_timer_* in a `[[maybe_unused]]` watchdog callback that never
// runs (no dispatcher registered). Keep just enough type/no-op macros
// to satisfy the compiler.
struct rte_timer {};
typedef void (*rte_timer_cb_t)(rte_timer *, void *);
#define rte_timer_init(x)          ((void)(x))
#define rte_timer_stop_sync(x)     ((void)(x))
#define rte_timer_reset(x, ticks, cb, arg) \
    ((void)(x), (void)(ticks), (void)(cb), (void)(arg))

#endif // !
