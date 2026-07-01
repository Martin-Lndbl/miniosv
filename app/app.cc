#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <api/minidpdk/dev.hh>
#include <api/minidpdk/mem.hh>
#include <minidpdk/defs.hh>
#include <minidpdk/mem.hh>
#include <minidpdk/net.hh>

using pool_ptr = std::unique_ptr<rte_pktmbuf_pool, decltype(&rte_mempool_free)>;

struct app_config {
  rte_ether_addr src;
  rte_ether_addr dst;
  uint32_t sip;
  uint32_t dip;
  uint32_t l4port;
  uint32_t mtu = 128;
};

struct port_info {
  uint16_t port_id = 0;
  rte_eth_dev *dev = nullptr;
  rte_ether_addr addr{};
  pool_ptr pool;

  port_info() : pool(nullptr, &rte_mempool_free) {}
};

static int probe_port(port_info &info) {
  static constexpr uint16_t kDescNum = 64;
  static constexpr uint32_t kMempoolCacheSize = 32;
  static constexpr uint32_t kPoolSize = 128;
  static constexpr uint32_t kDataRoomSize = 1536;

  info.dev = eth_os::get_eth_for_port(info.port_id);
  if (!info.dev) {
    std::cout << "FAIL: no device found for port " << info.port_id << std::endl;
    return ENODEV;
  }
  std::cout << "OK: device found" << std::endl;

  rte_eth_dev_info dinfo{};
  info.dev->get_dev_info(&dinfo);
  std::cout << "OK: device info retrieved" << std::endl;
  // std::cout << "  driver:       " << (dinfo.driver_name ? dinfo.driver_name :
  // "unknown") << std::endl;
  std::cout << "  max rx queues:" << dinfo.max_rx_queues << std::endl;
  std::cout << "  max tx queues:" << dinfo.max_tx_queues << std::endl;

  info.pool =
      pool_ptr(rte_pktmbuf_pool_create("probe-pool", kPoolSize,
                                       kMempoolCacheSize, 0, kDataRoomSize, 0),
               &rte_mempool_free);
  if (!info.pool) {
    std::cout << "FAIL: could not allocate packet pool" << std::endl;
    return ENOMEM;
  }
  std::cout << "OK: packet pool allocated" << std::endl;

  rte_eth_conf conf{};
  if (rte_eth_dev_configure(info.port_id, 1, 1, &conf)) {
    std::cout << "FAIL: device configure failed" << std::endl;
    return 1;
  }
  std::cout << "OK: device configured (1 rx queue, 1 tx queue)" << std::endl;

  uint16_t rx_desc = kDescNum, tx_desc = kDescNum;
  rte_eth_dev_adjust_nb_rx_tx_desc(info.port_id, &rx_desc, &tx_desc);

  rte_eth_rxconf rxconf{};
  if (rte_eth_rx_queue_setup(info.port_id, 0, rx_desc, 0, &rxconf,
                             info.pool.get())) {
    std::cout << "FAIL: rx queue setup failed" << std::endl;
    return 1;
  }
  std::cout << "OK: rx queue set up (" << rx_desc << " descriptors)"
            << std::endl;

  rte_eth_txconf txconf{};
  if (rte_eth_tx_queue_setup(info.port_id, 0, tx_desc, 0, &txconf)) {
    std::cout << "FAIL: tx queue setup failed" << std::endl;
    return 1;
  }
  std::cout << "OK: tx queue set up (" << tx_desc << " descriptors)"
            << std::endl;

  if (rte_eth_dev_start(info.port_id)) {
    std::cout << "FAIL: device start failed" << std::endl;
    return 1;
  }
  std::cout << "OK: device started" << std::endl;

  rte_eth_macaddr_get(info.port_id, &info.addr);
  // auto &a = info.addr.addr_bytes;
  // printf("OK: MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
  //        a[0], a[1], a[2], a[3], a[4], a[5]);

  rte_eth_stats stats{};
  info.dev->get_stats(&stats);
  std::cout << "OK: stats readable (rx: " << stats.ipackets << " pkts, "
            << stats.ibytes << " bytes)" << std::endl;

  return 0;
}

extern "C" void osv_app_main() {
  port_info info;
  for (unsigned i{0}; i < 64; ++i) {
    info.port_id = i;
    std::cout << "Probing port " << info.port_id << "..." << std::endl;
    int rc = probe_port(info);
    if (!rc) {
      std::cout << "RESULT: NIC probe succeeded (code " << rc << ")"
                << std::endl;
      info.dev->stop();
      break;
    }
  }
}
