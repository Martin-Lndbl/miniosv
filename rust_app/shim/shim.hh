// extern "C" bridge between the Rust port and the C++-only minidpdk
// API (rte_eth_dev exposes get_dev_info/get_stats/stop as virtual
// methods, and eth_os::get_eth_for_port() returns a C++ pointer —
// neither has a stable C ABI Rust can call directly).
//
// Design choice: every real DPDK/minidpdk struct (rte_eth_dev_info,
// rte_eth_conf, rte_eth_rxconf, rte_eth_txconf, rte_eth_stats) is
// constructed and read *inside* this shim. Only plain integers and
// opaque pointers cross into Rust, so Rust never needs to know their
// layout.
#pragma once
#include <cstdint>

extern "C" {

// Returns 1 if a device exists for `port_id`, else 0.
int shim_is_valid_port(uint16_t port_id);

// Fills *max_rx_queues / *max_tx_queues from rte_eth_dev_info.
// Returns 0 on success, -1 if the port doesn't exist.
int shim_get_dev_info(uint16_t port_id, uint16_t *max_rx_queues,
                       uint16_t *max_tx_queues);

// Wraps rte_pktmbuf_pool_create(); returns nullptr on failure.
void *shim_pktmbuf_pool_create(const char *name, uint32_t n,
                                uint32_t cache_size, uint16_t priv_size,
                                uint16_t data_room_size);
void shim_mempool_free(void *pool);

// Wraps rte_eth_dev_configure() with a zero-initialized rte_eth_conf.
int shim_eth_dev_configure(uint16_t port_id, uint16_t nb_rx_q,
                            uint16_t nb_tx_q);

void shim_adjust_nb_rx_tx_desc(uint16_t port_id, uint16_t *nb_rx_desc,
                                uint16_t *nb_tx_desc);

// Wraps rte_eth_rx_queue_setup() with a zero-initialized rte_eth_rxconf.
int shim_rx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc, void *mempool);

// Wraps rte_eth_tx_queue_setup() with a zero-initialized rte_eth_txconf.
int shim_tx_queue_setup(uint16_t port_id, uint16_t queue_id,
                         uint16_t nb_desc);

int shim_dev_start(uint16_t port_id);

// Wraps rte_eth_dev(port_id)->stop(). No-op if the port doesn't exist.
void shim_dev_stop(uint16_t port_id);

// Writes 6 bytes into addr_bytes.
void shim_macaddr_get(uint16_t port_id, uint8_t *addr_bytes);

// Fills the four counters from rte_eth_stats.
// Returns 0 on success, -1 if the port doesn't exist.
int shim_get_stats(uint16_t port_id, uint64_t *ipackets, uint64_t *opackets,
                    uint64_t *ibytes, uint64_t *obytes);

// Allocate an mbuf from `pool`, copy `len` bytes from `data` into it, and
// hand it to rte_eth_tx_burst(). If the burst returns 0 (queue full,
// device not ready), the mbuf is freed. Returns 0 on success, -1 on
// alloc/tx failure.
int shim_tx_packet(uint16_t port_id, uint16_t queue_id, void *pool,
                    const uint8_t *data, uint16_t len);

// Poll rte_eth_rx_burst() for one packet. If a packet is available,
// copy up to `max_len` bytes into `buf`, free the mbuf, and return the
// number of bytes copied (>0). Returns 0 if no packet was available.
int shim_rx_packet(uint16_t port_id, uint16_t queue_id, uint8_t *buf,
                    uint16_t max_len);

// --- Checksum-offload diagnostics ----------------------------------------

// Emit a one-shot summary of what checksum offloads the driver accepted
// at configure time and the aggregate `ol_flags` verdict the NIC produced
// across all received packets. Intended to be called once after the app
// finishes its work, so the counters don't get scrolled off the end of
// the 64 KB AWS console tail by response-body prints.
void shim_offload_report(void);

// --- Non-networking runtime hooks needed by rustls -----------------------

// Wall-clock seconds since the Unix epoch. Used only for TLS certificate
// validity checks; the accuracy just has to be within a cert's ~30 day
// slack, so time(NULL) at boot is fine.
uint64_t shim_time_seconds(void);

// Global allocator FFI. Rust's core+alloc stack needs a heap; we back it
// with OSv's C++ new/delete via malloc/free so we don't ship a second
// heap inside the Rust static library.
void *shim_malloc(uint64_t size);
void  shim_free(void *ptr);
void *shim_realloc(void *ptr, uint64_t size);

}  // extern "C"
