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
#include <minidpdk/defs.hh>
#include <minidpdk/net.hh>

namespace {

// Checksum offloads we ask ENA to do for us, on both directions. TX
// pushes IP/TCP/UDP checksum computation into the NIC; RX makes the
// NIC verify and report via mbuf->ol_flags. The set matches what smoltcp
// covers on the Rust side today (v4 only, no ipv6, no scatter, no TSO).
constexpr uint64_t kWantedTxOffloads =
    RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
    RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
    RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

constexpr uint64_t kWantedRxOffloads =
    RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
    RTE_ETH_RX_OFFLOAD_TCP_CKSUM |
    RTE_ETH_RX_OFFLOAD_UDP_CKSUM;

// Filled in by shim_eth_dev_configure based on what the device
// advertises via get_dev_info; queue_setup + tx_packet consult this to
// only try offloads the driver actually accepted.
uint64_t g_tx_offloads = 0;
uint64_t g_rx_offloads = 0;

// Accumulate the NIC's per-packet verdict on RX so shim_offload_report()
// can show whether HW verification actually ran. `..._good` counts
// packets where the NIC explicitly ran the check and it passed; `_bad`
// counts where the NIC ran the check and it failed (we dropped those);
// `_unknown` counts packets where we asked but the NIC declined to
// answer (non-IP/TCP/UDP, fragmented, etc.).
struct rx_verdict_counters {
    uint64_t ip_good = 0;
    uint64_t ip_bad = 0;
    uint64_t ip_unknown = 0;
    uint64_t l4_good = 0;
    uint64_t l4_bad = 0;
    uint64_t l4_unknown = 0;
    uint64_t total = 0;
};
rx_verdict_counters g_rx_verdict;

}  // namespace

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
  rte_eth_dev_info info;
  std::memset(&info, 0, sizeof(info));
  auto *dev = lookup_dev(port_id);
  if (!dev) return -1;
  dev->get_dev_info(&info);

  // Intersect what we want with what the device advertises. If the
  // driver doesn't support IP cksum we just leave it and smoltcp will
  // handle it in software; no reason to fail the whole configure.
  g_tx_offloads = kWantedTxOffloads & info.tx_offload_capa;
  g_rx_offloads = kWantedRxOffloads & info.rx_offload_capa;

  rte_eth_conf conf;
  std::memset(&conf, 0, sizeof(conf));
  conf.txmode.offloads = g_tx_offloads;
  conf.rxmode.offloads = g_rx_offloads;
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
  rxconf.offloads = g_rx_offloads;
  return rte_eth_rx_queue_setup(port_id, queue_id, nb_desc, SOCKET_ID_ANY,
                                 &rxconf, static_cast<rte_mempool *>(mempool));
}

int shim_tx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc) {
  rte_eth_txconf txconf;
  std::memset(&txconf, 0, sizeof(txconf));
  txconf.offloads = g_tx_offloads;
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

// Fill mbuf offload metadata (`l2_len` / `l3_len` / `ol_flags`) if the
// packet is an Ethernet II frame carrying IPv4 + (TCP|UDP) and the
// device advertised the matching offloads via g_tx_offloads. Zeros the
// IP header checksum and the L4 checksum field so the NIC — which
// knows to compute the pseudo-header from `l3_hdr_offset`/`l3_hdr_len`
// in the ENA tx meta — can fill them in on the fly.
static void ena_tx_offload_prepare(rte_mbuf *m, uint8_t *buf, uint16_t len) {
  m->ol_flags = 0;
  m->l2_len = 0;
  m->l3_len = 0;
  m->l4_len = 0;
  if (g_tx_offloads == 0 || len < 14 + 20) {
    return;
  }
  // Ethernet II: 12 bytes addresses + 2 bytes EtherType. No VLAN handling.
  const uint16_t ethertype = (uint16_t(buf[12]) << 8) | buf[13];
  if (ethertype != RTE_ETHER_TYPE_IPV4) {
    return;
  }
  uint8_t *ip = buf + 14;
  const uint8_t ihl_words = ip[0] & 0x0f;
  const uint16_t ip_hdr_len = uint16_t(ihl_words) * 4;
  if (ip_hdr_len < 20 || 14 + ip_hdr_len > len) {
    return;
  }

  m->l2_len = 14;
  m->l3_len = ip_hdr_len;
  m->ol_flags |= RTE_MBUF_F_TX_IPV4;

  if (g_tx_offloads & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) {
    m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
    // Zero the IPv4 header checksum (bytes 10..11 within the IP header).
    ip[10] = 0;
    ip[11] = 0;
  }

  const uint8_t proto = ip[9];
  const uint16_t l4_off = 14 + ip_hdr_len;
  if (proto == 6 /* TCP */ && l4_off + 20 <= len &&
      (g_tx_offloads & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)) {
    // TCP header: cksum at offset 16..17. Zero so the NIC fills it in.
    uint8_t *tcp = buf + l4_off;
    tcp[16] = 0;
    tcp[17] = 0;
    // data offset is high 4 bits of byte 12, in 32-bit words.
    const uint16_t tcp_hdr_len = uint16_t(tcp[12] >> 4) * 4;
    m->l4_len = tcp_hdr_len;
    m->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
  } else if (proto == 17 /* UDP */ && l4_off + 8 <= len &&
             (g_tx_offloads & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)) {
    uint8_t *udp = buf + l4_off;
    // UDP header cksum at offset 6..7. Zero so the NIC fills it.
    udp[6] = 0;
    udp[7] = 0;
    m->l4_len = 8;
    m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
  }
}

int shim_tx_packet(uint16_t port_id, uint16_t queue_id, void *pool,
                    const uint8_t *data, uint16_t len) {
  auto *mp = static_cast<rte_mempool *>(pool);
  rte_mbuf *m = rte_pktmbuf_alloc(mp);
  if (m == nullptr) {
    return -1;
  }
  uint8_t *dst = rte_pktmbuf_mtod(m, uint8_t *);
  std::memcpy(dst, data, len);
  m->data_len = len;
  m->pkt_len = len;
  m->nb_segs = 1;
  m->next = nullptr;

  // NIC offload: patch ol_flags / l2_len / l3_len and blank out the
  // cksum fields so hardware fills them.
  ena_tx_offload_prepare(m, dst, len);

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
  const uint64_t ol = m->ol_flags;

  // Bucket the NIC's per-packet verdict. Reported later via
  // shim_offload_report() so the summary survives the response-body
  // firehose.
  ++g_rx_verdict.total;
  switch (ol & RTE_MBUF_F_RX_IP_CKSUM_MASK) {
    case RTE_MBUF_F_RX_IP_CKSUM_GOOD:    ++g_rx_verdict.ip_good; break;
    case RTE_MBUF_F_RX_IP_CKSUM_BAD:     ++g_rx_verdict.ip_bad; break;
    default:                             ++g_rx_verdict.ip_unknown; break;
  }
  switch (ol & RTE_MBUF_F_RX_L4_CKSUM_MASK) {
    case RTE_MBUF_F_RX_L4_CKSUM_GOOD:    ++g_rx_verdict.l4_good; break;
    case RTE_MBUF_F_RX_L4_CKSUM_BAD:     ++g_rx_verdict.l4_bad; break;
    default:                             ++g_rx_verdict.l4_unknown; break;
  }

  // If the NIC verified checksums and flagged either L3 or L4 bad,
  // drop the packet here. smoltcp is configured to skip checksum
  // verification (ChecksumCapabilities::ignored) on the Rust side, so
  // it would otherwise happily hand corrupt bytes to rustls.
  const bool ip_bad =
      (ol & RTE_MBUF_F_RX_IP_CKSUM_MASK) == RTE_MBUF_F_RX_IP_CKSUM_BAD;
  const bool l4_bad =
      (ol & RTE_MBUF_F_RX_L4_CKSUM_MASK) == RTE_MBUF_F_RX_L4_CKSUM_BAD;
  if (ip_bad || l4_bad) {
    rte_pktmbuf_free(m);
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

void shim_offload_report(void) {
  printf("---- offload summary ----\n");
  printf("shim: tx_offloads accepted=%#lx (ipv4=%d tcp=%d udp=%d)\n",
         (unsigned long)g_tx_offloads,
         (g_tx_offloads & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) ? 1 : 0,
         (g_tx_offloads & RTE_ETH_TX_OFFLOAD_TCP_CKSUM) ? 1 : 0,
         (g_tx_offloads & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) ? 1 : 0);
  printf("shim: rx_offloads accepted=%#lx (ipv4=%d tcp=%d udp=%d)\n",
         (unsigned long)g_rx_offloads,
         (g_rx_offloads & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM) ? 1 : 0,
         (g_rx_offloads & RTE_ETH_RX_OFFLOAD_TCP_CKSUM) ? 1 : 0,
         (g_rx_offloads & RTE_ETH_RX_OFFLOAD_UDP_CKSUM) ? 1 : 0);
  printf("shim: rx verdict over %lu pkts: ip[good=%lu bad=%lu unk=%lu] l4[good=%lu bad=%lu unk=%lu]\n",
         (unsigned long)g_rx_verdict.total,
         (unsigned long)g_rx_verdict.ip_good,
         (unsigned long)g_rx_verdict.ip_bad,
         (unsigned long)g_rx_verdict.ip_unknown,
         (unsigned long)g_rx_verdict.l4_good,
         (unsigned long)g_rx_verdict.l4_bad,
         (unsigned long)g_rx_verdict.l4_unknown);
  printf("-------------------------\n");
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
