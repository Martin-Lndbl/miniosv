//! Stack-wide counters.
//!
//! These are cheap and stay in the build. Each one is an invariant that should
//! hold on every run, so a regression surfaces in the output rather than as a
//! mysteriously slower number. Anything that is a *policy* question -- whether
//! a 206 was expected, how many blocks a run planned -- belongs to the caller,
//! not here.

use core::sync::atomic::{AtomicU64, Ordering};

static MISROUTED_DROPS: AtomicU64 = AtomicU64::new(0);
static TX_ALLOC_FAIL: AtomicU64 = AtomicU64::new(0);
static TX_BURST_FAIL: AtomicU64 = AtomicU64::new(0);
static CONNS_ESTABLISHED: AtomicU64 = AtomicU64::new(0);
static CONNS_FAILED: AtomicU64 = AtomicU64::new(0);
static SYN_RETRIES: AtomicU64 = AtomicU64::new(0);
static SETUP_MS_TOTAL: AtomicU64 = AtomicU64::new(0);
static REQUESTS_SERVED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_REUSED: AtomicU64 = AtomicU64::new(0);

// Profiling: where a request's latency goes, and whether the worker that
// serves it actually gets the CPU. A worker polls without yielding, so the
// gap between consecutive polls should be microseconds; milliseconds mean it
// was descheduled, and a descheduled worker is one that is not draining the
// RX ring while the peer is still sending.
static QUEUE_WAIT_US_TOTAL: AtomicU64 = AtomicU64::new(0);
static WIRE_US_TOTAL: AtomicU64 = AtomicU64::new(0);
static BODY_BYTES_TOTAL: AtomicU64 = AtomicU64::new(0);
static POLL_ITERS: AtomicU64 = AtomicU64::new(0);
static POLL_GAP_US_TOTAL: AtomicU64 = AtomicU64::new(0);
static POLL_GAP_US_MAX: AtomicU64 = AtomicU64::new(0);
static POLL_GAPS_OVER_1MS: AtomicU64 = AtomicU64::new(0);
/// SYN to Established, in microseconds: one round trip, so this is the RTT to
/// the peer measured rather than assumed. `SETUP_MS_TOTAL` is the same
/// quantity at millisecond resolution, which is too coarse to be an RTT.
static SETUP_US_TOTAL: AtomicU64 = AtomicU64::new(0);
/// How much was waiting in the socket each time a connection drained it, and
/// the high-water mark. If the sender were filling the advertised window
/// these would be large; a drain that is always one segment means the peer is
/// sending one segment and waiting.
static RECV_DRAINS: AtomicU64 = AtomicU64::new(0);
static RECV_DRAIN_BYTES: AtomicU64 = AtomicU64::new(0);
static RECV_QUEUE_MAX: AtomicU64 = AtomicU64::new(0);
/// Time inside the shim's transmit call. An ACK costs one of these, so if
/// this is anywhere near the RTT then ACK transmission, not the network, is
/// what paces the peer.
static TX_CALLS: AtomicU64 = AtomicU64::new(0);
static TX_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static TX_NS_MAX: AtomicU64 = AtomicU64::new(0);
/// Request-on-the-wire to first response byte, and first byte to complete.
/// The first is S3's think time plus a round trip and is per-request whatever
/// the window is; only the second is transfer.
static TTFB_US_TOTAL: AtomicU64 = AtomicU64::new(0);
static TTFB_COUNT: AtomicU64 = AtomicU64::new(0);
static XFER_US_TOTAL: AtomicU64 = AtomicU64::new(0);
static XFER_COUNT: AtomicU64 = AtomicU64::new(0);
/// Extremes of the caller-supplied body buffer. A cap far from the sizes
/// httpfs actually asks for is the first sign the Range header was misread.
static BODY_CAP_MAX: AtomicU64 = AtomicU64::new(0);
static BODY_CAP_CALLS: AtomicU64 = AtomicU64::new(0);
/// Requests re-sent on a fresh connection after a reused one turned out to be
/// already closed. Expected to be small but nonzero: it is S3's keep-alive
/// timeout, not a fault.
static REQUESTS_RETRIED: AtomicU64 = AtomicU64::new(0);
/// Result published to submitter awake again: an unpark, the target cpu
/// noticing it, and a context switch. The one part of a request's life
/// neither `wire` nor `ttfb + xfer` covers, and the only candidate left for
/// the 8-9 ms by which DuckDB's blocking call exceeds `wire`.
static WAKE_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static WAKE_COUNT: AtomicU64 = AtomicU64::new(0);
static WAKE_NS_MAX: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Copy, Default)]
pub struct Stats {
    /// Packets discarded because they arrived on a queue that does not own the
    /// destination port. Source ports are chosen so this cannot happen; a
    /// nonzero value means the RSS model no longer matches the hardware.
    pub misrouted_drops: u64,
    /// Frames dropped before reaching the NIC: no mbuf available, or the TX
    /// ring refused them. Both are invisible to smoltcp, which believes it
    /// sent them.
    pub tx_alloc_fail: u64,
    pub tx_burst_fail: u64,
    pub conns_established: u64,
    pub conns_failed: u64,
    /// Extra SYNs beyond the first. Should be 0: a source port is chosen so
    /// the SYN-ACK provably comes back on the right queue, which leaves real
    /// packet loss as the only cause.
    pub syn_retries: u64,
    pub setup_ms_total: u64,
    /// Requests served on a connection that was already open, versus one that
    /// had to be dialled fresh. The gap between the two is the M3 win.
    pub requests_served: u64,
    pub requests_reused: u64,
    /// Summed over requests: time a request sat on a worker's queue before the
    /// worker picked it up, and time from pickup to completion. Queue wait
    /// near zero means slots were never the constraint and the latency is all
    /// on the wire.
    pub queue_wait_us_total: u64,
    pub wire_us_total: u64,
    pub body_bytes_total: u64,
    /// The worker poll loop. `gap` is the interval between consecutive
    /// iterations of one worker's loop; it bounds how long the RX ring can go
    /// undrained.
    pub poll_iters: u64,
    pub poll_gap_us_total: u64,
    pub poll_gap_us_max: u64,
    pub poll_gaps_over_1ms: u64,
    /// SYN to Established summed over connections, in microseconds. Divided by
    /// `conns_established` this is the round-trip time to the peer.
    pub setup_us_total: u64,
    /// Bytes waiting in the socket when a connection drained it. A mean near
    /// one MSS means the peer sends a segment and waits, whatever window we
    /// advertised.
    pub recv_drains: u64,
    pub recv_drain_bytes: u64,
    pub recv_queue_max: u64,
    /// Cost of the shim's transmit call, which every ACK pays.
    pub tx_calls: u64,
    pub tx_ns_total: u64,
    pub tx_ns_max: u64,
    /// Split of a request's life: waiting for S3 to start answering, then
    /// receiving. A large ttfb with a small transfer means latency, not
    /// bandwidth, and no window change will touch it.
    pub ttfb_us_total: u64,
    pub ttfb_count: u64,
    pub xfer_us_total: u64,
    pub xfer_count: u64,
    pub body_cap_max: u64,
    pub body_cap_calls: u64,
    pub requests_retried: u64,
    /// Publish-to-running for the submitting thread. Nanoseconds, not
    /// microseconds: a wake that took under a microsecond is the good case
    /// and a microsecond-resolution sum would round it away.
    pub wake_ns_total: u64,
    pub wake_count: u64,
    pub wake_ns_max: u64,
}

pub fn snapshot() -> Stats {
    Stats {
        misrouted_drops: MISROUTED_DROPS.load(Ordering::Relaxed),
        tx_alloc_fail: TX_ALLOC_FAIL.load(Ordering::Relaxed),
        tx_burst_fail: TX_BURST_FAIL.load(Ordering::Relaxed),
        conns_established: CONNS_ESTABLISHED.load(Ordering::Relaxed),
        conns_failed: CONNS_FAILED.load(Ordering::Relaxed),
        syn_retries: SYN_RETRIES.load(Ordering::Relaxed),
        setup_ms_total: SETUP_MS_TOTAL.load(Ordering::Relaxed),
        requests_served: REQUESTS_SERVED.load(Ordering::Relaxed),
        requests_reused: REQUESTS_REUSED.load(Ordering::Relaxed),
        queue_wait_us_total: QUEUE_WAIT_US_TOTAL.load(Ordering::Relaxed),
        wire_us_total: WIRE_US_TOTAL.load(Ordering::Relaxed),
        body_bytes_total: BODY_BYTES_TOTAL.load(Ordering::Relaxed),
        poll_iters: POLL_ITERS.load(Ordering::Relaxed),
        poll_gap_us_total: POLL_GAP_US_TOTAL.load(Ordering::Relaxed),
        poll_gap_us_max: POLL_GAP_US_MAX.load(Ordering::Relaxed),
        poll_gaps_over_1ms: POLL_GAPS_OVER_1MS.load(Ordering::Relaxed),
        setup_us_total: SETUP_US_TOTAL.load(Ordering::Relaxed),
        recv_drains: RECV_DRAINS.load(Ordering::Relaxed),
        recv_drain_bytes: RECV_DRAIN_BYTES.load(Ordering::Relaxed),
        recv_queue_max: RECV_QUEUE_MAX.load(Ordering::Relaxed),
        tx_calls: TX_CALLS.load(Ordering::Relaxed),
        tx_ns_total: TX_NS_TOTAL.load(Ordering::Relaxed),
        tx_ns_max: TX_NS_MAX.load(Ordering::Relaxed),
        ttfb_us_total: TTFB_US_TOTAL.load(Ordering::Relaxed),
        ttfb_count: TTFB_COUNT.load(Ordering::Relaxed),
        xfer_us_total: XFER_US_TOTAL.load(Ordering::Relaxed),
        xfer_count: XFER_COUNT.load(Ordering::Relaxed),
        body_cap_max: BODY_CAP_MAX.load(Ordering::Relaxed),
        body_cap_calls: BODY_CAP_CALLS.load(Ordering::Relaxed),
        requests_retried: REQUESTS_RETRIED.load(Ordering::Relaxed),
        wake_ns_total: WAKE_NS_TOTAL.load(Ordering::Relaxed),
        wake_count: WAKE_COUNT.load(Ordering::Relaxed),
        wake_ns_max: WAKE_NS_MAX.load(Ordering::Relaxed),
    }
}

pub(crate) fn misrouted_drop() {
    MISROUTED_DROPS.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn tx_alloc_fail() {
    TX_ALLOC_FAIL.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn tx_burst_fail() {
    TX_BURST_FAIL.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn conn_established(attempts: u16, setup_ms: i64, setup_ns: u64) {
    CONNS_ESTABLISHED.fetch_add(1, Ordering::Relaxed);
    SYN_RETRIES.fetch_add(attempts.saturating_sub(1) as u64, Ordering::Relaxed);
    SETUP_MS_TOTAL.fetch_add(setup_ms.max(0) as u64, Ordering::Relaxed);
    SETUP_US_TOTAL.fetch_add(setup_ns / 1_000, Ordering::Relaxed);
}
pub(crate) fn conn_failed(setup_ms: i64) {
    CONNS_FAILED.fetch_add(1, Ordering::Relaxed);
    SETUP_MS_TOTAL.fetch_add(setup_ms.max(0) as u64, Ordering::Relaxed);
}
pub(crate) fn request_started(reused: bool) {
    REQUESTS_SERVED.fetch_add(1, Ordering::Relaxed);
    if reused {
        REQUESTS_REUSED.fetch_add(1, Ordering::Relaxed);
    }
}

/// One completed request, with the two halves of its latency and what it
/// actually carried.
pub(crate) fn request_finished(queue_wait_ns: u64, wire_ns: u64, body_bytes: u64) {
    QUEUE_WAIT_US_TOTAL.fetch_add(queue_wait_ns / 1_000, Ordering::Relaxed);
    WIRE_US_TOTAL.fetch_add(wire_ns / 1_000, Ordering::Relaxed);
    BODY_BYTES_TOTAL.fetch_add(body_bytes, Ordering::Relaxed);
}

/// One iteration of a worker's poll loop, `gap_ns` after the previous one.
pub(crate) fn poll_tick(gap_ns: u64) {
    let us = gap_ns / 1_000;
    POLL_ITERS.fetch_add(1, Ordering::Relaxed);
    POLL_GAP_US_TOTAL.fetch_add(us, Ordering::Relaxed);
    POLL_GAP_US_MAX.fetch_max(us, Ordering::Relaxed);
    if gap_ns >= 1_000_000 {
        POLL_GAPS_OVER_1MS.fetch_add(1, Ordering::Relaxed);
    }
}

/// One drain of a socket's receive buffer: how much was sitting there.
pub(crate) fn recv_drain(bytes: usize, queued: usize) {
    RECV_DRAINS.fetch_add(1, Ordering::Relaxed);
    RECV_DRAIN_BYTES.fetch_add(bytes as u64, Ordering::Relaxed);
    RECV_QUEUE_MAX.fetch_max(queued as u64, Ordering::Relaxed);
}

/// One call into the shim's transmit path.
pub(crate) fn tx_call(ns: u64) {
    TX_CALLS.fetch_add(1, Ordering::Relaxed);
    TX_NS_TOTAL.fetch_add(ns, Ordering::Relaxed);
    TX_NS_MAX.fetch_max(ns, Ordering::Relaxed);
}

/// Request on the wire to first response byte.
pub(crate) fn ttfb(ns: u64) {
    TTFB_US_TOTAL.fetch_add(ns / 1_000, Ordering::Relaxed);
    TTFB_COUNT.fetch_add(1, Ordering::Relaxed);
}

/// First response byte to a complete response.
pub(crate) fn transfer(ns: u64) {
    XFER_US_TOTAL.fetch_add(ns / 1_000, Ordering::Relaxed);
    XFER_COUNT.fetch_add(1, Ordering::Relaxed);
}

/// One caller-supplied body buffer, by capacity.
pub(crate) fn body_buffer(cap: u64) {
    BODY_CAP_MAX.fetch_max(cap, Ordering::Relaxed);
    BODY_CAP_CALLS.fetch_add(1, Ordering::Relaxed);
}

/// One request re-sent on a fresh connection.
pub(crate) fn request_retried() {
    REQUESTS_RETRIED.fetch_add(1, Ordering::Relaxed);
}

/// One submitter, from its result being published to it running again.
pub(crate) fn wake(ns: u64) {
    WAKE_NS_TOTAL.fetch_add(ns, Ordering::Relaxed);
    WAKE_COUNT.fetch_add(1, Ordering::Relaxed);
    WAKE_NS_MAX.fetch_max(ns, Ordering::Relaxed);
}
