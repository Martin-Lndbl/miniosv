// rust_app/shim/shim.cpp
//
// Implementation of the extern "C" bridge declared in shim.h. See that
// file for the rationale: every real DPDK/minidpdk struct is built and
// read here, so only integers and opaque pointers ever cross into Rust.

#include "shim.hh"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <minidpdk/dev.hh>

namespace {

// eth_os::get_eth_for_port() is the only way to reach the C++
// rte_eth_dev object (and its virtual get_dev_info/get_stats/stop
// methods) for a given port. Centralize the null check here so every
// shim function gets the same "port doesn't exist" behavior.
rte_eth_dev *lookup_dev(uint16_t port_id) {
  return eth_os::get_eth_for_port(port_id);
}

}  // namespace

extern "C" {

int shim_is_valid_port(uint16_t port_id) {
  return lookup_dev(port_id) != nullptr ? 1 : 0;
}

int shim_get_dev_info(uint16_t port_id, uint16_t *max_rx_queues,
                       uint16_t *max_tx_queues) {
  rte_eth_dev *dev = lookup_dev(port_id);
  if (dev == nullptr) {
    return -1;
  }

  rte_eth_dev_info dev_info;
  std::memset(&dev_info, 0, sizeof(dev_info));
  dev->get_dev_info(&dev_info);

  *max_rx_queues = dev_info.max_rx_queues;
  *max_tx_queues = dev_info.max_tx_queues;
  return 0;
}

void *shim_pktmbuf_pool_create(const char *name, uint32_t n,
                                uint32_t cache_size, uint16_t priv_size,
                                uint16_t data_room_size) {
  rte_mempool *pool =
      rte_pktmbuf_pool_create(name, n, cache_size, priv_size, data_room_size,
                               SOCKET_ID_ANY);
  return static_cast<void *>(pool);
}

void shim_mempool_free(void *pool) {
  if (pool == nullptr) {
    return;
  }
  rte_mempool_free(static_cast<rte_mempool *>(pool));
}

int shim_eth_dev_configure(uint16_t port_id, uint16_t nb_rx_q,
                            uint16_t nb_tx_q) {
  rte_eth_conf conf;
  std::memset(&conf, 0, sizeof(conf));
  return rte_eth_dev_configure(port_id, nb_rx_q, nb_tx_q, &conf);
}

void shim_adjust_nb_rx_tx_desc(uint16_t port_id, uint16_t *nb_rx_desc,
                                uint16_t *nb_tx_desc) {
  // Best-effort: if the adjustment call fails, leave the caller's
  // requested descriptor counts untouched.
  rte_eth_dev_adjust_nb_rx_tx_desc(port_id, nb_rx_desc, nb_tx_desc);
}

int shim_rx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc, void *mempool) {
  rte_eth_rxconf rxconf;
  std::memset(&rxconf, 0, sizeof(rxconf));
  return rte_eth_rx_queue_setup(port_id, queue_id, nb_desc, SOCKET_ID_ANY,
                                 &rxconf, static_cast<rte_mempool *>(mempool));
}

int shim_tx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc) {
  rte_eth_txconf txconf;
  std::memset(&txconf, 0, sizeof(txconf));
  return rte_eth_tx_queue_setup(port_id, queue_id, nb_desc, SOCKET_ID_ANY,
                                 &txconf);
}

int shim_dev_start(uint16_t port_id) { return rte_eth_dev_start(port_id); }

void shim_dev_stop(uint16_t port_id) {
  rte_eth_dev *dev = lookup_dev(port_id);
  if (dev == nullptr) {
    return;
  }
  dev->stop();
}

void shim_macaddr_get(uint16_t port_id, uint8_t *addr_bytes) {
  rte_ether_addr addr;
  std::memset(&addr, 0, sizeof(addr));
  rte_eth_macaddr_get(port_id, &addr);
  std::memcpy(addr_bytes, addr.addr.data(), RTE_ETHER_ADDR_LEN);
}

int shim_get_stats(uint16_t port_id, uint64_t *ipackets, uint64_t *opackets,
                    uint64_t *ibytes, uint64_t *obytes) {
  rte_eth_dev *dev = lookup_dev(port_id);
  if (dev == nullptr) {
    return -1;
  }

  rte_eth_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  dev->get_stats(&stats);

  *ipackets = stats.ipackets;
  *opackets = stats.opackets;
  *ibytes = stats.ibytes;
  *obytes = stats.obytes;
  return 0;
}

int shim_tx_packet(uint16_t port_id, uint16_t queue_id, void *pool,
                    const uint8_t *data, uint16_t len) {
  auto *mp = static_cast<rte_mempool *>(pool);
  rte_mbuf *m = rte_pktmbuf_alloc(mp);
  if (m == nullptr) {
    return -1;
  }
  std::memcpy(rte_pktmbuf_mtod(m, uint8_t *), data, len);
  m->data_len = len;
  m->pkt_len = len;
  m->nb_segs = 1;
  m->next = nullptr;

  uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &m, 1);
  if (sent == 0) {
    rte_pktmbuf_free(m);
    return -1;
  }
  return 0;
}

int shim_rx_packet(uint16_t port_id, uint16_t queue_id, uint8_t *buf,
                    uint16_t max_len) {
  rte_mbuf *m = nullptr;
  uint16_t nb = rte_eth_rx_burst(port_id, queue_id, &m, 1);
  if (nb == 0 || m == nullptr) {
    return 0;
  }
  uint16_t len = m->data_len;
  if (len > max_len) {
    len = max_len;
  }
  std::memcpy(buf, rte_pktmbuf_mtod(m, uint8_t *), len);
  rte_pktmbuf_free(m);
  return static_cast<int>(len);
}

uint64_t shim_time_seconds(void) {
  return static_cast<uint64_t>(std::time(nullptr));
}

void *shim_malloc(uint64_t size) {
  return std::malloc(static_cast<size_t>(size));
}

void shim_free(void *ptr) { std::free(ptr); }

void *shim_realloc(void *ptr, uint64_t size) {
  return std::realloc(ptr, static_cast<size_t>(size));
}

// Stub for libc's variadic `syscall` used by `getrandom` (SYS_getrandom).
// OSv doesn't implement the Linux syscall trampoline, and no other Rust
// code path in this app should call syscall(). We treat the intended
// callsite (SYS_getrandom, buf, len) specially by filling from RDRAND;
// anything else returns -1/ENOSYS so failures are diagnosable rather
// than silent memory corruption.
#include <cerrno>
#include <cstdarg>
#include <cstdint>

namespace {
inline bool rdrand64_or_stall(uint64_t &out) {
    for (int i = 0; i < 10; ++i) {
        unsigned char ok;
        asm volatile("rdrand %0; setc %1" : "=r"(out), "=r"(ok));
        if (ok) return true;
    }
    return false;
}
}  // namespace

extern "C" long syscall(long number, ...) {
    constexpr long SYS_getrandom = 318;  // x86_64 Linux syscall number
    if (number != SYS_getrandom) {
        errno = ENOSYS;
        return -1;
    }
    va_list ap;
    va_start(ap, number);
    void *buf = va_arg(ap, void *);
    size_t len = va_arg(ap, size_t);
    (void)va_arg(ap, unsigned int);  // flags — ignored
    va_end(ap);

    auto *out = static_cast<uint8_t *>(buf);
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t v;
        if (!rdrand64_or_stall(v)) {
            errno = EIO;
            return -1;
        }
        std::memcpy(out + i, &v, 8);
        i += 8;
    }
    if (i < len) {
        uint64_t v;
        if (!rdrand64_or_stall(v)) {
            errno = EIO;
            return -1;
        }
        std::memcpy(out + i, &v, len - i);
    }
    return static_cast<long>(len);
}

}  // extern "C"
