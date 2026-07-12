#![no_std]
#![allow(non_camel_case_types, dead_code)]

extern crate alloc;

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_int;
use core::fmt::{self, Write};
use core::panic::PanicInfo;

use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::{dhcpv4, tcp};
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

use rustls::client::{ClientConfig, UnbufferedClientConnection};
use rustls::pki_types::{ServerName, UnixTime};
use rustls::time_provider::TimeProvider;
use rustls::unbuffered::{ConnectionState, UnbufferedStatus};
use rustls::RootCertStore;

// =====================================================================
// Global allocator — Rust's alloc crate calls into OSv's C malloc/free
// via the shim. rustls, its rustcrypto provider, webpki, and Arc<T> all
// need a heap; wiring one up before main() is the cheapest way to give
// them one.
// =====================================================================

extern "C" {
    fn shim_malloc(size: u64) -> *mut u8;
    fn shim_free(ptr: *mut u8);
    fn shim_realloc(ptr: *mut u8, size: u64) -> *mut u8;
    fn shim_time_seconds() -> u64;
}

struct ShimAllocator;

// Layouts alloc/dealloc pairs never carry alignment through the FFI —
// malloc gives 16-byte alignment on the OSv heap, which is enough for
// everything the TLS stack asks for (up to __m128 alignment). If a
// layout requires more, over-align: allocate `size + align - 1`, bump
// the returned pointer, stash the original for dealloc.
unsafe impl GlobalAlloc for ShimAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() <= 16 {
            unsafe { shim_malloc(layout.size() as u64) }
        } else {
            // Over-align by allocating extra headroom and storing the
            // original malloc'd pointer just before the aligned slot.
            let extra = layout.align() + core::mem::size_of::<*mut u8>();
            let raw = unsafe { shim_malloc((layout.size() + extra) as u64) };
            if raw.is_null() {
                return raw;
            }
            let raw_addr = raw as usize + core::mem::size_of::<*mut u8>();
            let aligned = (raw_addr + layout.align() - 1) & !(layout.align() - 1);
            unsafe {
                let slot = (aligned - core::mem::size_of::<*mut u8>()) as *mut *mut u8;
                *slot = raw;
            }
            aligned as *mut u8
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= 16 {
            unsafe { shim_free(ptr) };
        } else {
            unsafe {
                let slot = (ptr as usize - core::mem::size_of::<*mut u8>()) as *mut *mut u8;
                shim_free(*slot);
            }
        }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            unsafe { shim_realloc(ptr, new_size as u64) }
        } else {
            // Fall back to alloc/copy/dealloc when we've been over-aligning.
            let new_layout = Layout::from_size_align_unchecked(new_size, layout.align());
            let new_ptr = unsafe { self.alloc(new_layout) };
            if !new_ptr.is_null() {
                let copy = core::cmp::min(layout.size(), new_size);
                unsafe { core::ptr::copy_nonoverlapping(ptr, new_ptr, copy) };
                unsafe { self.dealloc(ptr, layout) };
            }
            new_ptr
        }
    }
}

#[global_allocator]
static GLOBAL: ShimAllocator = ShimAllocator;

// =====================================================================
// Minimal console output (no std::cout in no_std).
// Assumes a libc `write(2)` is linkable in the OSv environment.
// =====================================================================
extern "C" {
    fn write(fd: c_int, buf: *const u8, count: usize) -> isize;
}

struct Stdout;

impl Write for Stdout {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let bytes = s.as_bytes();
        let mut off = 0;
        while off < bytes.len() {
            let n = unsafe { write(1, bytes[off..].as_ptr(), bytes.len() - off) };
            if n <= 0 {
                return Err(fmt::Error);
            }
            off += n as usize;
        }
        Ok(())
    }
}

macro_rules! print {
    ($($arg:tt)*) => {{
        let _ = Stdout.write_fmt(format_args!($($arg)*));
    }};
}

macro_rules! println {
    () => {{
        let _ = Stdout.write_str("\n");
    }};
    ($($arg:tt)*) => {{
        let _ = Stdout.write_fmt(format_args!($($arg)*));
        let _ = Stdout.write_str("\n");
    }};
}

// =====================================================================
// minidpdk FFI surface (mirrors rust_app/shim/shim.hh)
// =====================================================================

/// Opaque handle to a minidpdk packet-mbuf pool. Never dereferenced on
/// the Rust side; only ever passed back into the shim.
#[repr(C)]
pub struct rte_pktmbuf_pool {
    _private: [u8; 0],
}

extern "C" {
    fn shim_is_valid_port(port_id: u16) -> c_int;
    fn shim_get_dev_info(port_id: u16, max_rx_queues: *mut u16, max_tx_queues: *mut u16) -> c_int;
    fn shim_pktmbuf_pool_create(
        name: *const u8,
        n: u32,
        cache_size: u32,
        priv_size: u16,
        data_room_size: u16,
    ) -> *mut rte_pktmbuf_pool;
    fn shim_mempool_free(pool: *mut rte_pktmbuf_pool);
    fn shim_eth_dev_configure(port_id: u16, nb_rx_q: u16, nb_tx_q: u16) -> c_int;
    fn shim_adjust_nb_rx_tx_desc(port_id: u16, nb_rx_desc: *mut u16, nb_tx_desc: *mut u16);
    fn shim_rx_queue_setup(
        port_id: u16,
        queue_id: u16,
        nb_desc: u16,
        mempool: *mut rte_pktmbuf_pool,
    ) -> c_int;
    fn shim_tx_queue_setup(port_id: u16, queue_id: u16, nb_desc: u16) -> c_int;
    fn shim_dev_start(port_id: u16) -> c_int;
    fn shim_dev_stop(port_id: u16);
    fn shim_macaddr_get(port_id: u16, addr_bytes: *mut u8);
    fn shim_get_stats(
        port_id: u16,
        ipackets: *mut u64,
        opackets: *mut u64,
        ibytes: *mut u64,
        obytes: *mut u64,
    ) -> c_int;

    // New: copy-in tx and copy-out rx (single packet).
    fn shim_tx_packet(
        port_id: u16,
        queue_id: u16,
        pool: *mut rte_pktmbuf_pool,
        data: *const u8,
        len: u16,
    ) -> c_int;
    fn shim_rx_packet(port_id: u16, queue_id: u16, buf: *mut u8, max_len: u16) -> c_int;
}

const ENODEV: c_int = 19;
const ENOMEM: c_int = 12;

// =====================================================================
// Packet pool RAII
// =====================================================================

struct PktPool(*mut rte_pktmbuf_pool);

impl Drop for PktPool {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { shim_mempool_free(self.0) };
        }
    }
}

// =====================================================================
// NIC probe + setup.
//
// Returns the MAC address on success. Unlike the old app that also
// stopped the device before returning, this leaves the port started so
// the caller can pump packets through it via the smoltcp interface.
// =====================================================================

fn probe_and_open(port_id: u16, pool_out: &mut Option<PktPool>) -> Result<[u8; 6], c_int> {
    const DESC_NUM: u16 = 64;
    const MEMPOOL_CACHE_SIZE: u32 = 32;
    const POOL_SIZE: u32 = 256;
    const DATA_ROOM_SIZE: u16 = 1536;

    if unsafe { shim_is_valid_port(port_id) } == 0 {
        return Err(ENODEV);
    }
    println!("OK: device found on port {}", port_id);

    let mut max_rx: u16 = 0;
    let mut max_tx: u16 = 0;
    if unsafe { shim_get_dev_info(port_id, &mut max_rx, &mut max_tx) } != 0 {
        return Err(ENODEV);
    }
    println!("  max rx queues:{}  max tx queues:{}", max_rx, max_tx);

    let name = b"http-pool\0";
    let p = unsafe {
        shim_pktmbuf_pool_create(
            name.as_ptr(),
            POOL_SIZE,
            MEMPOOL_CACHE_SIZE,
            0,
            DATA_ROOM_SIZE,
        )
    };
    if p.is_null() {
        return Err(ENOMEM);
    }
    *pool_out = Some(PktPool(p));
    println!("OK: packet pool allocated");

    if unsafe { shim_eth_dev_configure(port_id, 1, 1) } != 0 {
        return Err(1);
    }
    println!("OK: device configured (1 rx, 1 tx queue)");

    let mut rx_desc = DESC_NUM;
    let mut tx_desc = DESC_NUM;
    unsafe { shim_adjust_nb_rx_tx_desc(port_id, &mut rx_desc, &mut tx_desc) };

    let pool_ptr = pool_out.as_ref().unwrap().0;
    if unsafe { shim_rx_queue_setup(port_id, 0, rx_desc, pool_ptr) } != 0 {
        return Err(1);
    }
    println!("OK: rx queue set up ({} descriptors)", rx_desc);

    if unsafe { shim_tx_queue_setup(port_id, 0, tx_desc) } != 0 {
        return Err(1);
    }
    println!("OK: tx queue set up ({} descriptors)", tx_desc);

    if unsafe { shim_dev_start(port_id) } != 0 {
        return Err(1);
    }
    println!("OK: device started");

    let mut mac = [0u8; 6];
    unsafe { shim_macaddr_get(port_id, mac.as_mut_ptr()) };
    println!(
        "OK: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
    Ok(mac)
}

// =====================================================================
// smoltcp Device backed by minidpdk tx/rx bursts.
//
// The RxToken/TxToken split forces us to hand out both tokens with
// non-overlapping borrows. We reuse the trick from the earlier
// loopback experiment: the RX buffer lives in a `static mut` so the
// RxToken holds an immutable slice unrelated to `&mut self`, while
// the TxToken holds the `&mut self` reference for tx_scratch access.
// =====================================================================

const MTU: usize = 1514;

// Ethernet frame staging buffer for RX. Only one DpdkDevice instance
// exists at a time (`osv_app_main` is single-threaded), so a single
// static slot is enough.
static mut RX_STAGING: [u8; MTU] = [0u8; MTU];
static mut RX_STAGING_LEN: usize = 0;

struct DpdkDevice {
    port_id: u16,
    pool: *mut rte_pktmbuf_pool,
    tx_scratch: [u8; MTU],
}

struct DpdkRxToken<'a> {
    buf: &'a [u8],
}

struct DpdkTxToken<'a> {
    dev: &'a mut DpdkDevice,
}

impl<'a> RxToken for DpdkRxToken<'a> {
    fn consume<R, F>(self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        let r = f(self.buf);
        unsafe {
            RX_STAGING_LEN = 0;
        }
        r
    }
}

impl<'a> TxToken for DpdkTxToken<'a> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let n = core::cmp::min(len, self.dev.tx_scratch.len());
        let r = f(&mut self.dev.tx_scratch[..n]);
        unsafe {
            shim_tx_packet(
                self.dev.port_id,
                0,
                self.dev.pool,
                self.dev.tx_scratch.as_ptr(),
                n as u16,
            );
        }
        r
    }
}

impl Device for DpdkDevice {
    type RxToken<'a>
        = DpdkRxToken<'a>
    where
        Self: 'a;
    type TxToken<'a>
        = DpdkTxToken<'a>
    where
        Self: 'a;

    fn receive(&mut self, _t: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        use core::ptr::{addr_of, addr_of_mut};
        let staging_ptr: *mut u8 = addr_of_mut!(RX_STAGING) as *mut u8;
        let staging_cap: u16 = MTU as u16;
        let len = unsafe {
            if RX_STAGING_LEN == 0 {
                let n = shim_rx_packet(self.port_id, 0, staging_ptr, staging_cap);
                if n <= 0 {
                    return None;
                }
                RX_STAGING_LEN = n as usize;
            }
            RX_STAGING_LEN
        };
        // SAFETY: RX_STAGING is only mutated from receive(), and the
        // returned slice is invalidated by the consume() path resetting
        // RX_STAGING_LEN before the next receive() would touch the buffer.
        let buf: &[u8] =
            unsafe { core::slice::from_raw_parts(addr_of!(RX_STAGING) as *const u8, len) };
        Some((DpdkRxToken { buf }, DpdkTxToken { dev: self }))
    }

    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        Some(DpdkTxToken { dev: self })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.max_transmission_unit = MTU;
        c.medium = Medium::Ethernet;
        c
    }
}

// =====================================================================
// DHCP + HTTPS GET to Cloudflare's DNS-over-HTTPS front-end.
//
// The IP/gateway/subnet we get from AWS's DHCP server. The HTTPS
// target is 1.1.1.1:443, whose cert covers `one.one.one.one`. Fixed
// public IP → no DNS needed. GET / returns a small HTML body which is
// enough to prove: TCP connect → TLS handshake → HTTP request/reply
// over TLS → close_notify → TCP close.
// =====================================================================

const TARGET_IP: Ipv4Address = Ipv4Address::new(1, 1, 1, 1);
const TARGET_PORT: u16 = 443;
const TARGET_SNI: &str = "one.one.one.one";
const LOCAL_PORT: u16 = 49152;

const REQUEST: &[u8] = b"GET / HTTP/1.0\r\n\
                         Host: one.one.one.one\r\n\
                         User-Agent: minidpdk-smoltcp/0.1\r\n\
                         Connection: close\r\n\
                         \r\n";

// smoltcp fake-clock stride: one "ms" every CLOCK_STRIDE poll iterations.
// STATS_STRIDE tunes how often we dump NIC counters. ITER_BUDGET is a
// safety net so a hung run doesn't spin forever.
const CLOCK_STRIDE: u64 = 5_000;
const STATS_STRIDE: u64 = 500_000;
const ITER_BUDGET: u64 = 5_000_000_000;

fn tick_clock(clock_ms: &mut i64, iter: &mut u64) {
    *iter = iter.wrapping_add(1);
    if iter.is_multiple_of(CLOCK_STRIDE) {
        *clock_ms = clock_ms.wrapping_add(1);
    }
}

fn dump_stats(port_id: u16, iter: u64, clock_ms: i64) {
    let (mut i_pkts, mut o_pkts, mut i_bytes, mut o_bytes) = (0u64, 0u64, 0u64, 0u64);
    unsafe {
        shim_get_stats(
            port_id,
            &mut i_pkts,
            &mut o_pkts,
            &mut i_bytes,
            &mut o_bytes,
        );
    }
    println!(
        "stats: iter={} clock={}ms rx={} pkts/{} B  tx={} pkts/{} B",
        iter, clock_ms, i_pkts, i_bytes, o_pkts, o_bytes
    );
}

/// Runs a DHCPv4 exchange until the server hands us an address, then
/// installs it (with default route + subnet mask) on the interface.
/// Returns the acquired (address, gateway) tuple.
fn dhcp_acquire(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    dhcp_handle: smoltcp::iface::SocketHandle,
    port_id: u16,
) -> Option<(smoltcp::wire::Ipv4Cidr, Ipv4Address)> {
    let mut clock_ms: i64 = 0;
    let mut iter: u64 = 0;
    println!("DHCP: requesting lease...");
    loop {
        iface.poll(Instant::from_millis(clock_ms), dev, sockets);

        let s = sockets.get_mut::<dhcpv4::Socket>(dhcp_handle);
        match s.poll() {
            Some(dhcpv4::Event::Configured(cfg)) => {
                let a = cfg.address;
                let o = a.address().octets();
                println!(
                    "DHCP: address {}.{}.{}.{}/{}",
                    o[0], o[1], o[2], o[3], a.prefix_len()
                );
                let router = cfg.router.unwrap_or(Ipv4Address::new(0, 0, 0, 0));
                let r = router.octets();
                println!("DHCP: gateway {}.{}.{}.{}", r[0], r[1], r[2], r[3]);
                iface.update_ip_addrs(|addrs| {
                    let _ = addrs.push(IpCidr::Ipv4(a));
                });
                if let Some(gw) = cfg.router {
                    let _ = iface.routes_mut().add_default_ipv4_route(gw);
                }
                return Some((a, router));
            }
            Some(dhcpv4::Event::Deconfigured) => {
                println!("DHCP: deconfigured (lease lost)");
            }
            None => {}
        }

        tick_clock(&mut clock_ms, &mut iter);
        if iter.is_multiple_of(STATS_STRIDE) {
            dump_stats(port_id, iter, clock_ms);
        }
        // DHCP is fast on AWS; a fraction of ITER_BUDGET is more than enough.
        if iter > ITER_BUDGET / 10 {
            println!("DHCP: timeout ({} iters, {} fake-ms)", iter, clock_ms);
            return None;
        }
    }
}

// ---------------------------------------------------------------------
// RDRAND-backed CryptoRng. `rustls-rustcrypto` needs a `CryptoRng` for
// ClientHello.random and the ephemeral keys; every c5.large / c7i.large
// CPU has RDRAND, so we don't need to plumb entropy through the shim.
// ---------------------------------------------------------------------
struct RdRandRng;

fn rdrand64() -> Option<u64> {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        let mut out: u64 = 0;
        // Intel SDM: retry up to 10 times before treating RDRAND as unavailable.
        for _ in 0..10 {
            let ok: u8;
            core::arch::asm!(
                "rdrand {r}",
                "setc {ok}",
                r = out(reg) out,
                ok = out(reg_byte) ok,
                options(nostack, nomem)
            );
            if ok != 0 {
                return Some(out);
            }
        }
        None
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        None
    }
}

impl rand_core::RngCore for RdRandRng {
    fn next_u32(&mut self) -> u32 {
        rdrand64().expect("rdrand unavailable") as u32
    }
    fn next_u64(&mut self) -> u64 {
        rdrand64().expect("rdrand unavailable")
    }
    fn fill_bytes(&mut self, dest: &mut [u8]) {
        let mut i = 0;
        while i + 8 <= dest.len() {
            let v = rdrand64().expect("rdrand unavailable");
            dest[i..i + 8].copy_from_slice(&v.to_ne_bytes());
            i += 8;
        }
        let remaining = dest.len() - i;
        if remaining > 0 {
            let v = rdrand64().expect("rdrand unavailable");
            let tail = v.to_ne_bytes();
            dest[i..].copy_from_slice(&tail[..remaining]);
        }
    }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
        self.fill_bytes(dest);
        Ok(())
    }
}

impl rand_core::CryptoRng for RdRandRng {}

// Route the `getrandom` crate through RDRAND too. Every RustCrypto
// dependency below rustls calls `getrandom::getrandom()` at some point;
// on our target, its default backend is the Linux `getrandom(2)`
// syscall, which OSv doesn't provide. With the `custom` feature turned
// on in Cargo.toml, this macro replaces the backend with our own.
fn osv_getrandom(dest: &mut [u8]) -> Result<(), getrandom::Error> {
    let mut i = 0;
    while i + 8 <= dest.len() {
        let v = rdrand64().ok_or_else(|| {
            getrandom::Error::from(core::num::NonZeroU32::new(1).unwrap())
        })?;
        dest[i..i + 8].copy_from_slice(&v.to_ne_bytes());
        i += 8;
    }
    let remaining = dest.len() - i;
    if remaining > 0 {
        let v = rdrand64().ok_or_else(|| {
            getrandom::Error::from(core::num::NonZeroU32::new(1).unwrap())
        })?;
        let tail = v.to_ne_bytes();
        dest[i..].copy_from_slice(&tail[..remaining]);
    }
    Ok(())
}

getrandom::register_custom_getrandom!(osv_getrandom);

// ---------------------------------------------------------------------
// TLS pump. Drives a rustls `ClientConnection` on top of the same
// smoltcp TCP socket as before:
//
//   1. Drain rustls.wants_write() into the TCP tx buffer.
//   2. Copy TCP rx bytes into rustls via read_tls / process_new_packets.
//   3. Once handshake is done, write plaintext HTTP request into rustls
//      (which encrypts and hands us record bytes back on the next tx
//      pump), and read plaintext response out.
//   4. On peer close: send close_notify, wait for socket close.
//
// Everything shares the fake-ms clock we already use for smoltcp; the
// same ITER_BUDGET/STATS cadence applies.
// ---------------------------------------------------------------------

// Wall-clock provider for cert-validity checks. rustls needs `Sync +
// Send + Debug`; our impl is a ZST so it's trivially all three.
#[derive(Debug)]
struct ShimTimeProvider;

impl TimeProvider for ShimTimeProvider {
    fn current_time(&self) -> Option<UnixTime> {
        Some(UnixTime::since_unix_epoch(core::time::Duration::from_secs(unsafe {
            shim_time_seconds()
        })))
    }
}

fn make_client_config() -> Arc<ClientConfig> {
    let mut roots = RootCertStore::empty();
    roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    let provider = rustls_rustcrypto::provider();
    let cfg = ClientConfig::builder_with_details(
        Arc::new(provider),
        Arc::new(ShimTimeProvider),
    )
    .with_safe_default_protocol_versions()
    .expect("rustls: default protocol versions")
    .with_root_certificates(roots)
    .with_no_client_auth();
    Arc::new(cfg)
}

// Scratch buffers for the unbuffered pump. rustls records max out at
// 16 KB + overhead. incoming grows with pipelined records; outgoing
// grows with handshake bursts. 24 KB each is enough headroom for a
// full TLS 1.3 handshake plus one HTTP round trip.
const TLS_BUF_CAP: usize = 24 * 1024;

fn https_get(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    tcp_handle: smoltcp::iface::SocketHandle,
    port_id: u16,
) {
    // --- TCP connect --------------------------------------------------
    {
        let s = sockets.get_mut::<tcp::Socket>(tcp_handle);
        if let Err(_) = s.connect(iface.context(), (TARGET_IP, TARGET_PORT), LOCAL_PORT) {
            println!("FAIL: tcp connect() rejected");
            return;
        }
    }
    let o = TARGET_IP.octets();
    println!(
        "connecting to {}.{}.{}.{}:{} (SNI {}) ...",
        o[0], o[1], o[2], o[3], TARGET_PORT, TARGET_SNI
    );

    // --- rustls unbuffered client -------------------------------------
    let server_name = match ServerName::try_from(TARGET_SNI) {
        Ok(n) => n.to_owned(),
        Err(_) => {
            println!("FAIL: tls invalid ServerName");
            return;
        }
    };
    let cfg = make_client_config();
    let mut conn = match UnbufferedClientConnection::new(cfg, server_name) {
        Ok(c) => c,
        Err(e) => {
            println!("FAIL: tls UnbufferedClientConnection::new: {:?}", e);
            return;
        }
    };

    // Unbuffered-API buffers: `incoming` accumulates ciphertext read
    // from TCP until rustls can decode complete records; `outgoing`
    // holds ciphertext rustls asked us to send until TCP tx queue
    // drains it.
    let mut incoming: Vec<u8> = Vec::with_capacity(TLS_BUF_CAP);
    let mut outgoing: Vec<u8> = Vec::with_capacity(TLS_BUF_CAP);
    let mut request_queued = false;
    let mut bytes_received: usize = 0;
    let mut clock_ms: i64 = 0;
    let mut iter: u64 = 0;
    let mut last_tcp_state: tcp::State = tcp::State::Closed;
    let mut handshake_done = false;
    let mut close_notify_sent = false;

    loop {
        iface.poll(Instant::from_millis(clock_ms), dev, sockets);
        let s = sockets.get_mut::<tcp::Socket>(tcp_handle);

        let state = s.state();
        if state != last_tcp_state {
            println!("tcp state: {:?}", state);
            last_tcp_state = state;
        }

        // 1) Drain any queued outgoing ciphertext into the TCP tx buffer.
        if !outgoing.is_empty() && s.can_send() {
            match s.send_slice(&outgoing) {
                Ok(n) if n > 0 => {
                    outgoing.drain(..n);
                }
                _ => {}
            }
        }

        // 2) Pull ciphertext from TCP into `incoming` for rustls.
        if s.can_recv() {
            let _ = s.recv(|buf| {
                incoming.extend_from_slice(buf);
                (buf.len(), ())
            });
        }

        // 3) Advance rustls's state machine.
        //    We keep looping through process_tls_records until either
        //    it asks for more incoming (BlockedHandshake) or we hit a
        //    terminal state.
        let mut progress = true;
        while progress {
            progress = false;
            let UnbufferedStatus { discard, state } = conn.process_tls_records(&mut incoming);
            let st = match state {
                Ok(st) => st,
                Err(e) => {
                    println!("FAIL: tls process_tls_records: {:?}", e);
                    return;
                }
            };
            match st {
                ConnectionState::ReadTraffic(mut rt) => {
                    while let Some(rec) = rt.next_record() {
                        match rec {
                            Ok(rec) => {
                                bytes_received += rec.payload.len();
                                match core::str::from_utf8(rec.payload) {
                                    Ok(txt) => print!("{}", txt),
                                    Err(_) => print!("<{} non-utf8 bytes>", rec.payload.len()),
                                }
                            }
                            Err(e) => {
                                println!("FAIL: tls record: {:?}", e);
                                return;
                            }
                        }
                    }
                    progress = true;
                }
                ConnectionState::ReadEarlyData(_) => {
                    // We never sent 0-RTT data, so this shouldn't fire.
                    // Ignore.
                }
                ConnectionState::EncodeTlsData(mut et) => {
                    // rustls wants us to emit a handshake fragment.
                    // Grow outgoing to fit; encode straight into it.
                    let head = outgoing.len();
                    outgoing.resize(TLS_BUF_CAP, 0);
                    let n = match et.encode(&mut outgoing[head..]) {
                        Ok(n) => n,
                        Err(e) => {
                            println!("FAIL: tls encode: {:?}", e);
                            return;
                        }
                    };
                    outgoing.truncate(head + n);
                    progress = true;
                }
                ConnectionState::TransmitTlsData(tt) => {
                    // rustls signals: "the bytes I asked you to encode
                    // are ready to go on the wire". We already appended
                    // them into `outgoing`; nothing to do beyond ACKing.
                    tt.done();
                    progress = true;
                }
                ConnectionState::BlockedHandshake => {
                    // Need more incoming; wait for the TCP layer to
                    // deliver more bytes on the next iface.poll().
                }
                ConnectionState::WriteTraffic(mut wt) => {
                    if !handshake_done {
                        println!("OK: tls handshake complete");
                        handshake_done = true;
                    }

                    if !request_queued {
                        let head = outgoing.len();
                        outgoing.resize(head + REQUEST.len() + 128, 0);
                        let n = match wt.encrypt(REQUEST, &mut outgoing[head..]) {
                            Ok(n) => n,
                            Err(e) => {
                                println!("FAIL: tls encrypt request: {:?}", e);
                                return;
                            }
                        };
                        outgoing.truncate(head + n);
                        println!(
                            "OK: encrypted {} bytes of HTTP request into {} bytes of ciphertext",
                            REQUEST.len(),
                            n
                        );
                        request_queued = true;
                        progress = true;
                    } else if !close_notify_sent && !s.can_send() {
                        // Reserved slot to enqueue close_notify once
                        // we've received the response and want to end.
                    }
                }
                ConnectionState::PeerClosed | ConnectionState::Closed => {
                    if !close_notify_sent {
                        // Best-effort close_notify path is via WriteTraffic;
                        // by the time we're here we've already seen peer
                        // close, so just log and let TCP finish.
                        close_notify_sent = true;
                    }
                }
                _ => {}
            }
            incoming.drain(..discard);
        }

        // 4) End condition: peer closed the TLS session and TCP is done.
        if handshake_done && request_queued && !s.is_active() && outgoing.is_empty() {
            println!();
            println!(
                "OK: HTTPS exchange complete ({} bytes plaintext received)",
                bytes_received
            );
            return;
        }

        tick_clock(&mut clock_ms, &mut iter);
        if iter.is_multiple_of(STATS_STRIDE) {
            dump_stats(port_id, iter, clock_ms);
        }
        if iter > ITER_BUDGET {
            println!(
                "TIMEOUT after {} iters (fake clock {} ms, received {} bytes)",
                iter, clock_ms, bytes_received
            );
            return;
        }
    }
}

fn run_net(mac: [u8; 6], mut dev: DpdkDevice) {
    let config = Config::new(EthernetAddress(mac).into());
    let mut iface = Interface::new(config, &mut dev, Instant::from_millis(0));

    static mut TCP_RX: [u8; 8192] = [0u8; 8192];
    static mut TCP_TX: [u8; 4096] = [0u8; 4096];
    let tcp_sock = tcp::Socket::new(
        tcp::SocketBuffer::new(unsafe { &mut TCP_RX[..] }),
        tcp::SocketBuffer::new(unsafe { &mut TCP_TX[..] }),
    );

    static mut STORAGE: [SocketStorage; 2] = [SocketStorage::EMPTY; 2];
    let mut sockets = unsafe { SocketSet::new(&mut STORAGE[..]) };
    let tcp_handle = sockets.add(tcp_sock);
    let dhcp_handle = sockets.add(dhcpv4::Socket::new());

    let port_id = dev.port_id;
    if dhcp_acquire(&mut iface, &mut dev, &mut sockets, dhcp_handle, port_id).is_none() {
        return;
    }

    https_get(&mut iface, &mut dev, &mut sockets, tcp_handle, port_id);
}

// =====================================================================
// Entry point
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn osv_app_main() {
    let mut pool: Option<PktPool> = None;
    let mut mac_opt: Option<[u8; 6]> = None;
    let mut port_id_used: u16 = 0;

    for i in 0u16..64 {
        println!("Probing port {}...", i);
        match probe_and_open(i, &mut pool) {
            Ok(mac) => {
                mac_opt = Some(mac);
                port_id_used = i;
                break;
            }
            Err(_) => {
                // Drop any half-built pool before trying the next port.
                pool = None;
            }
        }
    }

    match (mac_opt, pool.as_ref()) {
        (Some(mac), Some(pool_ref)) => {
            let dev = DpdkDevice {
                port_id: port_id_used,
                pool: pool_ref.0,
                tx_scratch: [0u8; MTU],
            };
            run_net(mac, dev);
            unsafe { shim_dev_stop(port_id_used) };
        }
        _ => {
            println!("FAIL: no usable NIC found");
        }
    }

    loop {
        core::hint::spin_loop();
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
