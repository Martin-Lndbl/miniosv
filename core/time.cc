#include "processor.hh"
#include <osv/clock.hh>
#include <api/minidpdk/time.hh>
#include <cstdint>
#include <unistd.h>

uint64_t rte_get_timer_cycles() { return processor::ticks(); }

uint64_t rte_get_timer_hz() {
  auto nanos = clock::get()->processor_to_nano(NS_PER_S);
  return static_cast<uint64_t>(NS_PER_S * NS_PER_S) / nanos;
}

void rte_delay_us_block(unsigned int us) {
  const auto end = rte_get_timer_cycles() + us * rte_get_timer_hz() / US_PER_S;
  while (rte_get_timer_cycles() < end)
#ifdef __x86_64__
    ; // __asm __volatile("pause");
#endif
#ifdef __aarch64__
  __asm __volatile("isb sy");
#endif
}

void rte_delay_us_sleep(unsigned int us) { rte_delay_us_block(us); }
