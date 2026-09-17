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
static REQUESTS_SERVED: AtomicU64 = AtomicU64::new(0);
static REQUESTS_REUSED: AtomicU64 = AtomicU64::new(0);

/// SYN on the wire to Established.
static SETUP_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static SETUP_NS_MAX: AtomicU64 = AtomicU64::new(0);
static SETUP_HIST: [AtomicU64; BUCKETS] = [const { AtomicU64::new(0) }; BUCKETS];

/// Building a connection, before its SYN can go out. The count lives in the
/// histogram, so there is no separate counter to keep in step with it.
static DIAL_NS_TOTAL: AtomicU64 = AtomicU64::new(0);
static DIAL_NS_MAX: AtomicU64 = AtomicU64::new(0);
static DIAL_HIST: [AtomicU64; BUCKETS] = [const { AtomicU64::new(0) }; BUCKETS];

/// Log2 histogram over microseconds: bucket `k > 0` holds `[2^(k-1), 2^k)`,
/// bucket 0 holds everything under a microsecond. Coarse on purpose -- it
/// costs a shift and one relaxed increment, and a percentile good to a factor
/// of two is all that is needed to tell 80 us from 20 ms.
pub(crate) const BUCKETS: usize = 32;

pub(crate) fn bucket(us: u64) -> usize {
    if us == 0 {
        return 0;
    }
    (64 - us.leading_zeros() as usize).min(BUCKETS - 1)
}

fn record(hist: &[AtomicU64; BUCKETS], total: &AtomicU64, max: &AtomicU64, ns: u64) {
    total.fetch_add(ns, Ordering::Relaxed);
    max.fetch_max(ns, Ordering::Relaxed);
    hist[bucket(ns / 1_000)].fetch_add(1, Ordering::Relaxed);
}

fn load_hist(hist: &[AtomicU64; BUCKETS]) -> [u64; BUCKETS] {
    let mut out = [0u64; BUCKETS];
    for (o, h) in out.iter_mut().zip(hist.iter()) {
        *o = h.load(Ordering::Relaxed);
    }
    out
}

/// Interpolated linearly inside the bucket the answer lands in, so a p50 in a
/// well-populated bucket is not reported as that bucket's floor.
pub(crate) fn percentile(hist: &[u64; BUCKETS], p: u64) -> u64 {
    let count: u64 = hist.iter().sum();
    if count == 0 {
        return 0;
    }
    let target = (count * p).div_ceil(100).max(1);
    let mut seen = 0u64;
    for (k, &n) in hist.iter().enumerate() {
        if n == 0 {
            continue;
        }
        if seen + n >= target {
            if k == 0 {
                return 0;
            }
            let lo = 1u64 << (k - 1);
            return lo + lo * (target - seen - 1) / n;
        }
        seen += n;
    }
    0
}

/// One measured duration, summarised. Microseconds throughout: the old
/// millisecond clock quantised a ~1 ms handshake to 0 or 1, which at eight
/// workers threw away most of the signal.
#[derive(Debug, Clone, Copy, Default)]
pub struct Dist {
    pub n: u64,
    pub us_avg: u64,
    pub us_p50: u64,
    pub us_p90: u64,
    pub us_max: u64,
}

fn dist(hist: &[AtomicU64; BUCKETS], total: &AtomicU64, max: &AtomicU64) -> Dist {
    let h = load_hist(hist);
    let n: u64 = h.iter().sum();
    Dist {
        n,
        us_avg: if n == 0 {
            0
        } else {
            total.load(Ordering::Relaxed) / n / 1_000
        },
        us_p50: percentile(&h, 50),
        us_p90: percentile(&h, 90),
        us_max: max.load(Ordering::Relaxed) / 1_000,
    }
}

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
    /// SYN on the wire to Established -- one round trip, so this is the
    /// measured RTT to the peer. smoltcp does not expose its retransmit count,
    /// so a lost SYN shows up here instead: its first retransmit is a second
    /// out, which no healthy handshake can reach.
    pub setup: Dist,
    /// CPU burned building a connection -- the rustls session and its key
    /// share -- before its SYN could be put on the wire. Separate from
    /// `setup` because they answer different questions and only one of them is
    /// about the network.
    pub dial: Dist,
    /// Requests served on a connection that was already open, versus one that
    /// had to be dialled fresh. The gap between the two is the M3 win.
    pub requests_served: u64,
    pub requests_reused: u64,
}

pub fn snapshot() -> Stats {
    Stats {
        misrouted_drops: MISROUTED_DROPS.load(Ordering::Relaxed),
        tx_alloc_fail: TX_ALLOC_FAIL.load(Ordering::Relaxed),
        tx_burst_fail: TX_BURST_FAIL.load(Ordering::Relaxed),
        conns_established: CONNS_ESTABLISHED.load(Ordering::Relaxed),
        conns_failed: CONNS_FAILED.load(Ordering::Relaxed),
        setup: dist(&SETUP_HIST, &SETUP_NS_TOTAL, &SETUP_NS_MAX),
        dial: dist(&DIAL_HIST, &DIAL_NS_TOTAL, &DIAL_NS_MAX),
        requests_served: REQUESTS_SERVED.load(Ordering::Relaxed),
        requests_reused: REQUESTS_REUSED.load(Ordering::Relaxed),
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
pub(crate) fn conn_established(setup_ns: u64) {
    CONNS_ESTABLISHED.fetch_add(1, Ordering::Relaxed);
    record(&SETUP_HIST, &SETUP_NS_TOTAL, &SETUP_NS_MAX, setup_ns);
}
pub(crate) fn conn_failed(setup_ns: u64) {
    CONNS_FAILED.fetch_add(1, Ordering::Relaxed);
    record(&SETUP_HIST, &SETUP_NS_TOTAL, &SETUP_NS_MAX, setup_ns);
}
pub(crate) fn conn_dialled(dial_ns: u64) {
    record(&DIAL_HIST, &DIAL_NS_TOTAL, &DIAL_NS_MAX, dial_ns);
}
pub(crate) fn request_started(reused: bool) {
    REQUESTS_SERVED.fetch_add(1, Ordering::Relaxed);
    if reused {
        REQUESTS_REUSED.fetch_add(1, Ordering::Relaxed);
    }
}
