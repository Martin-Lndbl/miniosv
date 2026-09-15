/*
 * mininet: a userspace network stack for miniOSv.
 *
 * No sockets, no file descriptors, no libc networking -- mininet polls the
 * NIC queues itself, the way modules/miniext drives NVMe itself. Implemented
 * in Rust (modules/mininet/rust): smoltcp for TCP/IP, rustls for TLS, over
 * minidpdk. This header is the whole C++ surface.
 *
 * One endpoint per image. A worker owns an RSS queue outright, and the source
 * ports it may use are a function of the *peer's* address -- that is what
 * lets it poll one queue with nothing shared and nothing locked. Serving a
 * second host would mean a second set of workers; not implemented.
 *
 * There is no resolver: up() takes the address the caller already knows.
 */

#ifndef MININET_HH
#define MININET_HH

#include <cstddef>
#include <cstdint>

namespace mininet {

// Return codes. Negative on failure, so `rc < 0` reads like an errno test --
// though these are not errnos; none of them has a sensible POSIX spelling.
// strerror() turns one into a string.
enum : int {
    OK                = 0,
    E_NO_DEVICE       = -1,   // no usable NIC: absent, or configure/start refused
    E_NO_MEMORY       = -2,
    E_RSS             = -3,   // steering model unreadable; see the note above
    E_DHCP            = -4,
    E_ARP             = -5,
    E_NO_PORTS        = -6,
    E_CONNECT         = -7,
    E_SYN_TIMEOUT     = -8,
    E_TLS             = -9,
    E_BAD_RESPONSE    = -10,  // no status line, endless head, or chunked
    E_BUFFER_TOO_SMALL = -11, // body larger than the buffer; nothing overran
    E_NOT_UP          = -12,
    E_BAD_ARGUMENT    = -13,
};

struct config {
    //! `Host:` header and TLS server name. Must be the name the certificate is
    //! issued for, even though `address` is what gets dialled.
    const char *host;
    //! Dotted quad, e.g. "3.5.216.240".
    const char *address;
    //! 0 dials plain HTTP on port 80, which isolates the network stack from
    //! the record layer.
    int tls;
    //! RSS queues, and so worker threads, to ask for. Clamped to what the
    //! device advertises -- ENA caps queue count per instance size.
    uint32_t workers;
    //! Connection slots per worker. The concurrency ceiling is
    //! `workers * conns_per_worker` requests in flight; past that, callers
    //! queue.
    uint32_t conns_per_worker;
    //! Per-socket receive buffer, or 0 for the default. This dominates the
    //! stack's memory: workers * conns_per_worker * rx_buffer.
    uint64_t rx_buffer;
};

//! Header values are fixed arrays, not pointers, so a response crosses by
//! value and there is nothing to free. An ETag longer than this is truncated;
//! callers compare ETags for equality and a truncated one simply fails to
//! match, which is the safe direction to be wrong in.
enum : size_t {
    ETAG_MAX = 128,
    DATE_MAX = 64,
};

struct response {
    //! HTTP status, or 0 if no head was read.
    uint32_t status;
    //! What the head said the body was; 0 when it said nothing.
    uint64_t content_length;
    //! Bytes written into the caller's buffer.
    uint64_t bytes;
    //! Content-Range, when there was one. The three numbers mean nothing when
    //! this is 0.
    uint32_t has_range;
    uint64_t range_first;
    uint64_t range_last;
    //! 0 when the server sent `*` for the total.
    uint64_t range_total;
    //! NUL-terminated; empty when the header was absent.
    char etag[ETAG_MAX];
    char last_modified[DATE_MAX];
};

//! Configure and start the NIC, take a DHCP lease, resolve the gateway, and
//! spawn one worker thread per queue pinned to its own CPU. Call once, at
//! startup; calling again while up is a no-op.
//!
//! The workers poll without yielding, so they want CPUs to themselves: leave
//! `workers` below the core count and tell the application about the rest.
int up(const config &c);

bool is_up();

//! The host the stack was brought up for, or nullptr when it is not up.
//!
//! One endpoint per image, so a caller that wants a different host has to
//! refuse rather than silently fetch from this one. See the note at the top.
const char *host();

//! Send `head` and write the response body into `buf`.
//!
//! `head` is the complete request head -- request line, headers, blank line --
//! rendered by the caller; mininet carries HTTP, it does not build it.
//!
//! Blocks. The calling thread is parked, not spun, so it does not compete for
//! the CPU a worker is using. Any thread may call this, and many may at once.
//!
//! A body larger than `cap` is E_BUFFER_TOO_SMALL: the excess is dropped
//! rather than written past the end, so this is reported as the failure it
//! is rather than as a short read.
int get(const char *head, size_t head_len, void *buf, size_t cap, response *out);

//! Never null.
const char *strerror(int rc);

struct conn_stats {
    //! Requests served, and how many of those reused a connection the peer
    //! hadn't closed instead of paying for a fresh handshake.
    uint64_t requests_served;
    uint64_t requests_reused;
    //! Summed over requests: time queued waiting for a free connection slot,
    //! and time from a worker picking the request up to the response being
    //! complete. Queue wait near zero means slots were never the constraint.
    uint64_t queue_wait_us_total;
    uint64_t wire_us_total;
    uint64_t body_bytes_total;
    //! The worker poll loop. A worker polls without yielding, so the gap
    //! between iterations should be microseconds; milliseconds mean it lost
    //! the CPU, and a worker that is off-CPU is not draining the RX ring.
    uint64_t poll_iters;
    uint64_t poll_gap_us_total;
    uint64_t poll_gap_us_max;
    uint64_t poll_gaps_over_1ms;
    //! Frames lost before smoltcp could see them. `nic_imissed` is the device
    //! dropping for want of a descriptor, which the stack above cannot
    //! observe -- and which the peer sees as congestion.
    uint64_t misrouted_drops;
    uint64_t tx_alloc_fail;
    uint64_t tx_burst_fail;
    uint64_t nic_ipackets;
    uint64_t nic_ibytes;
    uint64_t nic_imissed;
    uint64_t nic_ierrors;
    uint64_t nic_rx_nombuf;
    //! SYN to Established is one round trip, so setup_us_total divided by
    //! conns_established is the measured RTT to the peer.
    uint64_t conns_established;
    uint64_t conns_failed;
    uint64_t syn_retries;
    uint64_t setup_us_total;
    //! Bytes waiting in the socket when a connection drained it. A mean near
    //! one MSS means the peer sends a segment and waits, whatever window we
    //! advertised -- which makes throughput one MSS per round trip.
    uint64_t recv_drains;
    uint64_t recv_drain_bytes;
    uint64_t recv_queue_max;
    //! Cost of one transmit call, which every ACK pays.
    uint64_t tx_calls;
    uint64_t tx_ns_total;
    uint64_t tx_ns_max;
    //! Request on the wire to first response byte, then first byte to done.
    //! A large ttfb against a small transfer is latency, not bandwidth.
    uint64_t ttfb_us_total;
    uint64_t ttfb_count;
    uint64_t xfer_us_total;
    uint64_t xfer_count;
    //! Largest body buffer a caller ever claimed, and how many it handed
    //! over. A cap unlike the sizes httpfs asks for means a misread Range.
    uint64_t body_cap_max;
    uint64_t body_cap_calls;
    //! Requests re-sent on a fresh connection because a reused one turned
    //! out to be already closed. S3's keep-alive timeout, not a fault.
    uint64_t requests_retried;
    //! From the worker publishing a result to the submitting thread running
    //! again: an unpark, the target cpu noticing, and a context switch.
    //! Nanoseconds, because the good case is under a microsecond. This is the
    //! only part of a request neither `wire_us_total` nor `ttfb + xfer`
    //! covers, and the caller's own blocking time exceeds `wire` by 8-9 ms
    //! per request -- so either this accounts for that or nothing does.
    uint64_t wake_ns_total;
    uint64_t wake_count;
    uint64_t wake_ns_max;
};

conn_stats stats();

} // namespace mininet

#endif /* MININET_HH */
