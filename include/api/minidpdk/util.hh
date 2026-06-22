#ifndef BYPASS_UTIL_H
#define BYPASS_UTIL_H

#include <atomic>
#include <features.h>
#include <osv/barrier.hh>
#include <osv/irqlock.hh>
#include <osv/mmio.hh>
#include <osv/spinlock.h>
#include <sys/types.h>

#define rte_wmb wmb
#define rte_rmb rmb
#define rte_mb mb
#define rte_io_wmb mb
#define RTE_SET_USED(x) (void)(x)

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define rte_prefetch0(a) __builtin_prefetch(a, 0, 0)
#define rte_prefetch0_write(a) __builtin_prefetch(a, 1, 0)

using rte_atomic32_t = std::atomic<uint32_t>;
using rte_atomic64_t = std::atomic<uint32_t>;

#define rte_atomic32_set(a, v) std::atomic_store(a, v)
#define rte_atomic32_read(a) std::atomic_load(a)
#define rte_atomic32_inc(a) std::atomic_fetch_add(a, 1)
#define rte_atomic32_dec(a) std::atomic_fetch_add(a, -1)

#define rte_atomic64_init(a) std::atomic_init(a, 0)
#define rte_atomic64_read(a) std::atomic_load(a)
#define rte_atomic64_inc(a) std::atomic_fetch_add(a, 1)

#define RTE_CACHE_LINE_SIZE 64
#define __rte_cache_aligned __attribute__((__aligned__(RTE_CACHE_LINE_SIZE)))
#define __rte_unused __attribute__((__unused__))
#define __rte_always_inline __attribute__((__always_inline__))

#define rte_spinlock_t np_spinlock_t
#define rte_spinlock_init np_spinlock_init

static __inline void rte_spinlock_lock(rte_spinlock_t *lock) {
  irq_lock.lock();
  np_spin_lock(lock);
}

static __inline void rte_spinlock_unlock(rte_spinlock_t *lock) {
  np_spin_unlock(lock);
  irq_lock.unlock();
}

static __inline void rte_write32(uint32_t val, mmioaddr_t addr) {
  rte_wmb();
  mmio_setl(addr, val);
}

static __inline void rte_write32_relaxed(uint32_t val, mmioaddr_t addr) {
  mmio_setl(addr, val);
}

static __inline uint32_t rte_read32(const mmioaddr_t addr) {
  auto val = mmio_getl(addr);
  rte_rmb();
  return val;
}

static __inline uint32_t rte_read32_relaxed(const mmioaddr_t addr) {
  return mmio_getl(addr);
}

static __inline void rte_write64_relaxed(uint64_t val, mmioaddr_t addr) {
  mmio_setq(addr, val);
}
#endif // !BYPASS_MEM_H
