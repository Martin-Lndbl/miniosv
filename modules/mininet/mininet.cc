/*
 * mininet.hh over the Rust crate's C ABI (rust/src/capi.rs).
 *
 * Nothing but naming: the crate exports flat `mininet_*` symbols because that
 * is what `#[no_mangle]` can promise, and callers want a namespace and
 * references. The structs are laid out identically on both sides -- capi.rs
 * declares them #[repr(C)] against this file -- so they are passed through,
 * not converted.
 */

#include "mininet.hh"

#include <cstddef>

extern "C" {

struct mininet_config_abi {
    const char *host;
    const char *address;
    int tls;
    uint32_t workers;
    uint32_t conns_per_worker;
    uint64_t rx_buffer;
};

struct mininet_response_abi {
    uint32_t status;
    uint64_t content_length;
    uint64_t bytes;
    uint32_t has_range;
    uint64_t range_first;
    uint64_t range_last;
    uint64_t range_total;
    char etag[128];
    char last_modified[64];
};

struct mininet_conn_stats_abi {
    uint64_t requests_served;
    uint64_t requests_reused;
    uint64_t queue_wait_us_total;
    uint64_t wire_us_total;
    uint64_t body_bytes_total;
    uint64_t poll_iters;
    uint64_t poll_gap_us_total;
    uint64_t poll_gap_us_max;
    uint64_t poll_gaps_over_1ms;
    uint64_t misrouted_drops;
    uint64_t tx_alloc_fail;
    uint64_t tx_burst_fail;
    uint64_t nic_ipackets;
    uint64_t nic_ibytes;
    uint64_t nic_imissed;
    uint64_t nic_ierrors;
    uint64_t nic_rx_nombuf;
    uint64_t conns_established;
    uint64_t conns_failed;
    uint64_t syn_retries;
    uint64_t setup_us_total;
    uint64_t recv_drains;
    uint64_t recv_drain_bytes;
    uint64_t recv_queue_max;
    uint64_t tx_calls;
    uint64_t tx_ns_total;
    uint64_t tx_ns_max;
    uint64_t ttfb_us_total;
    uint64_t ttfb_count;
    uint64_t xfer_us_total;
    uint64_t xfer_count;
    uint64_t body_cap_max;
    uint64_t body_cap_calls;
    uint64_t requests_retried;
    uint64_t wake_ns_total;
    uint64_t wake_count;
    uint64_t wake_ns_max;
};

int mininet_up(const mininet_config_abi *cfg);
int mininet_is_up(void);
const char *mininet_host(void);
int mininet_get(const char *head, uint64_t head_len, void *buf, uint64_t cap,
                mininet_response_abi *out);
const char *mininet_strerror(int rc);
mininet_conn_stats_abi mininet_conn_stats(void);
}

static_assert(sizeof(mininet::config) == sizeof(mininet_config_abi),
              "mininet::config must match capi.rs");
static_assert(sizeof(mininet::response) == sizeof(mininet_response_abi),
              "mininet::response must match capi.rs");
static_assert(offsetof(mininet::response, etag) == offsetof(mininet_response_abi, etag),
              "mininet::response field order must match capi.rs");
static_assert(offsetof(mininet::response, last_modified) ==
                  offsetof(mininet_response_abi, last_modified),
              "mininet::response field order must match capi.rs");
static_assert(sizeof(mininet::conn_stats) == sizeof(mininet_conn_stats_abi),
              "mininet::conn_stats must match capi.rs");

namespace mininet {

int up(const config &c)
{
	return mininet_up(reinterpret_cast<const mininet_config_abi *>(&c));
}

bool is_up()
{
	return mininet_is_up() != 0;
}

const char *host()
{
	return mininet_host();
}

int get(const char *head, size_t head_len, void *buf, size_t cap, response *out)
{
	return mininet_get(head, static_cast<uint64_t>(head_len), buf,
	                   static_cast<uint64_t>(cap),
	                   reinterpret_cast<mininet_response_abi *>(out));
}

const char *strerror(int rc)
{
	return mininet_strerror(rc);
}

conn_stats stats()
{
	auto s = mininet_conn_stats();
	conn_stats out {};
	out.requests_served = s.requests_served;
	out.requests_reused = s.requests_reused;
	out.queue_wait_us_total = s.queue_wait_us_total;
	out.wire_us_total = s.wire_us_total;
	out.body_bytes_total = s.body_bytes_total;
	out.poll_iters = s.poll_iters;
	out.poll_gap_us_total = s.poll_gap_us_total;
	out.poll_gap_us_max = s.poll_gap_us_max;
	out.poll_gaps_over_1ms = s.poll_gaps_over_1ms;
	out.misrouted_drops = s.misrouted_drops;
	out.tx_alloc_fail = s.tx_alloc_fail;
	out.tx_burst_fail = s.tx_burst_fail;
	out.nic_ipackets = s.nic_ipackets;
	out.nic_ibytes = s.nic_ibytes;
	out.nic_imissed = s.nic_imissed;
	out.nic_ierrors = s.nic_ierrors;
	out.nic_rx_nombuf = s.nic_rx_nombuf;
	out.conns_established = s.conns_established;
	out.conns_failed = s.conns_failed;
	out.syn_retries = s.syn_retries;
	out.setup_us_total = s.setup_us_total;
	out.recv_drains = s.recv_drains;
	out.recv_drain_bytes = s.recv_drain_bytes;
	out.recv_queue_max = s.recv_queue_max;
	out.tx_calls = s.tx_calls;
	out.tx_ns_total = s.tx_ns_total;
	out.tx_ns_max = s.tx_ns_max;
	out.ttfb_us_total = s.ttfb_us_total;
	out.ttfb_count = s.ttfb_count;
	out.xfer_us_total = s.xfer_us_total;
	out.xfer_count = s.xfer_count;
	out.body_cap_max = s.body_cap_max;
	out.body_cap_calls = s.body_cap_calls;
	out.requests_retried = s.requests_retried;
	out.wake_ns_total = s.wake_ns_total;
	out.wake_count = s.wake_count;
	out.wake_ns_max = s.wake_ns_max;
	return out;
}

} // namespace mininet
