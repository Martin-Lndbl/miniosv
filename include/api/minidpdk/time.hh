#ifndef BYPASS_TIME_H
#define BYPASS_TIME_H

#include <cstdint>
#include <osv/callout.h>

static constexpr uint64_t NS_PER_S = 1e9;
static constexpr uint64_t NS_PER_US = 1e3;
static constexpr uint64_t US_PER_S = 1e6;

uint64_t rte_get_timer_cycles(void);

uint64_t rte_get_timer_hz(void);

void rte_delay_us_block(unsigned int us);

void rte_delay_us_sleep(unsigned int us);

using rte_timer = callout;

typedef void (*rte_timer_cb_t)(rte_timer *, void *);
#define rte_timer_init(x) callout_init(x, true)
#define rte_timer_stop_sync(x) callout_drain(x)
#define rte_timer_reset(x, ticks, cb, arg)                                     \
  callout_reset(x, ns2ticks(ticks), cb, arg)

#define TSECOND (1000000000L)
#define TMILISECOND (1000000L)
#define hz (1000L)
#define ticks2ns(ticks) (ticks * (TSECOND / hz))
#define ns2ticks(ns) (ns / (TSECOND / hz))

#endif // !
