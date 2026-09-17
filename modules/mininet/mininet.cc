/*
 * mininet.hh over the Rust crate's C ABI (rust/src/capi.rs). The structs are
 * laid out identically on both sides and passed through.
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
    uint64_t requests_retried;
    uint64_t requests_done;
    uint64_t queue_ns_total;
    uint64_t wire_ns_total;
    uint64_t body_bytes;
    uint64_t ttfb_ns_total;
    uint64_t ttfb_n;
    uint64_t xfer_ns_total;
    uint64_t xfer_n;
    uint64_t poll_iters;
    uint64_t poll_gap_ns_total;
    uint64_t poll_gap_ns_max;
    uint64_t poll_gaps_over_1ms;
    uint64_t poll_busy_ns;
    uint64_t poll_active_iters;
    uint64_t poll_work_ns;
    uint64_t wake_n;
    uint64_t wake_ns_total;
    uint64_t wake_ns_max;
    uint64_t get_calls;
    uint64_t get_ns_total;
    uint64_t conns_established;
    uint64_t conns_failed;
    uint64_t setup_us_avg;
    uint64_t setup_us_max;
    uint64_t dial_us_avg;
    uint64_t misrouted_drops;
    uint64_t tx_alloc_fail;
    uint64_t tx_burst_fail;
    uint64_t nic_ipackets;
    uint64_t nic_ibytes;
    uint64_t nic_imissed;
    uint64_t nic_ierrors;
    uint64_t nic_rx_nombuf;
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
static_assert(sizeof(mininet::conn_stats) == sizeof(mininet_conn_stats_abi),
              "mininet::conn_stats must match capi.rs");
static_assert(offsetof(mininet::response, etag) == offsetof(mininet_response_abi, etag),
              "mininet::response field order must match capi.rs");
static_assert(offsetof(mininet::response, last_modified) ==
                  offsetof(mininet_response_abi, last_modified),
              "mininet::response field order must match capi.rs");

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
	conn_stats out;
	static_assert(sizeof(out) == sizeof(s), "layout");
	__builtin_memcpy(&out, &s, sizeof(out));
	return out;
}

} // namespace mininet
