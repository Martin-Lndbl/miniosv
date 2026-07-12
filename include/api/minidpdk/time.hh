#ifndef BYPASS_TIME_H
#define BYPASS_TIME_H

#include <cstdint>

static constexpr uint64_t NS_PER_S = 1e9;
static constexpr uint64_t NS_PER_US = 1e3;
static constexpr uint64_t US_PER_S = 1e6;

uint64_t rte_get_timer_cycles(void);

uint64_t rte_get_timer_hz(void);

void rte_delay_us_block(unsigned int us);

void rte_delay_us_sleep(unsigned int us);

#endif // !
