#![no_std]
#![allow(non_camel_case_types)]

extern crate alloc;

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_int, c_void};
use core::fmt::{self, Write};
use core::panic::PanicInfo;
use core::ptr;

use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::phy::{ChecksumCapabilities, Device, DeviceCapabilities, Medium, RxToken, TxToken};
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
    fn shim_time_ns() -> u64;
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
    fn shim_offload_report();
    fn shim_thread_spawn(
        f: extern "C" fn(*mut c_void),
        arg: *mut c_void,
        cpu_id: c_int,
    ) -> *mut c_void;
    fn shim_thread_join(handle: *mut c_void);
    fn shim_macaddr_get(port_id: u16, addr_bytes: *mut u8);

    // Zero-copy TX: alloc → write into `data[..cap]` → shim_mbuf_tx.
    fn shim_mbuf_alloc_tx(
        pool: *mut rte_pktmbuf_pool,
        out_handle: *mut *mut c_void,
        out_cap: *mut u16,
    ) -> *mut u8;
    fn shim_mbuf_tx(
        port_id: u16,
        queue_id: u16,
        handle: *mut c_void,
        len: u16,
    ) -> c_int;
    fn shim_mbuf_free(handle: *mut c_void);

    // Zero-copy RX: burst one packet; the shim hands back the mbuf
    // handle + data pointer + length. Rust owns the handle until it
    // calls shim_mbuf_free.
    fn shim_mbuf_rx_burst(
        port_id: u16,
        queue_id: u16,
        out_handle: *mut *mut c_void,
        out_data: *mut *const u8,
        out_len: *mut u16,
    ) -> c_int;
}

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
// NIC probe + setup. Returns (owned pool, MAC) if the port exists and
// was successfully started; None otherwise. Leaves the port running so
// the caller can pump packets through it.
// =====================================================================

/// Set up the NIC with `n_queues` RX + `n_queues` TX queues (RSS on
/// the TCP/IPv4 4-tuple when n_queues > 1), configure MTU to jumbo,
/// and start the device. Returns the shared packet pool and MAC.
fn probe_and_open(port_id: u16, n_queues: u16) -> Option<(PktPool, [u8; 6])> {
    const DATA_ROOM_SIZE: u16 = 1536;
    const DESC_NUM: u16 = 1024;
    const POOL_SIZE: u32 = 4096;
    const MEMPOOL_CACHE_SIZE: u32 = 64;

    if unsafe { shim_is_valid_port(port_id) } == 0 {
        return None;
    }
    println!("OK: device found on port {}", port_id);

    let mut max_rx: u16 = 0;
    let mut max_tx: u16 = 0;
    if unsafe { shim_get_dev_info(port_id, &mut max_rx, &mut max_tx) } != 0 {
        return None;
    }
    println!("  max rx queues:{}  max tx queues:{}", max_rx, max_tx);

    let name = b"bench-pool\0";
    let raw = unsafe {
        shim_pktmbuf_pool_create(name.as_ptr(), POOL_SIZE, MEMPOOL_CACHE_SIZE, 0, DATA_ROOM_SIZE)
    };
    if raw.is_null() {
        return None;
    }
    let pool = PktPool(raw);
    println!("OK: packet pool ({} mbufs, {} B each) allocated", POOL_SIZE, DATA_ROOM_SIZE);

    if unsafe { shim_eth_dev_configure(port_id, n_queues, n_queues) } != 0 {
        return None;
    }
    println!("OK: device configured ({} rx, {} tx queues)", n_queues, n_queues);

    let mut rx_desc = DESC_NUM;
    let mut tx_desc = DESC_NUM;
    unsafe { shim_adjust_nb_rx_tx_desc(port_id, &mut rx_desc, &mut tx_desc) };

    for q in 0..n_queues {
        if unsafe { shim_rx_queue_setup(port_id, q, rx_desc, pool.0) } != 0 {
            return None;
        }
        if unsafe { shim_tx_queue_setup(port_id, q, tx_desc) } != 0 {
            return None;
        }
    }
    println!(
        "OK: rx+tx queues set up ({} descriptors each)",
        rx_desc
    );

    if unsafe { shim_dev_start(port_id) } != 0 {
        return None;
    }
    println!("OK: device started");

    let mut mac = [0u8; 6];
    unsafe { shim_macaddr_get(port_id, mac.as_mut_ptr()) };
    println!(
        "OK: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
    Some((pool, mac))
}

// =====================================================================
// smoltcp Device backed by minidpdk tx/rx bursts, zero-copy on both
// directions: TX writes straight into an mbuf's data area (no
// Rust-side scratch, no memcpy), RX carries the mbuf pointer through
// the RxToken and frees it after smoltcp/rustls has consumed the
// bytes. rustls's unbuffered `next_record` writes plaintext into a
// separate buffer anyway, so the mbuf can go back to the pool as soon
// as consume() returns.
// =====================================================================

// smoltcp's view of the max Ethernet frame size, including L2 header.
const MTU: usize = 1514;

struct DpdkDevice {
    port_id: u16,
    queue_id: u16,
    pool: *mut rte_pktmbuf_pool,
    // A one-shot synthetic Ethernet frame (typically an ARP reply
    // fabricated from a gateway MAC learned on another queue) returned
    // on the next `receive()` call before we poll the real NIC. Used
    // to seed smoltcp's neighbor cache on worker queues without doing
    // an ARP exchange that RSS would misroute.
    pending_synth: Option<Vec<u8>>,
}

/// Holds either an owned mbuf handle from `shim_mbuf_rx_burst`, or a
/// synthetic frame owned by an inline `Vec`. `consume()` hands the
/// packet's bytes to smoltcp/rustls; `Drop` covers the "smoltcp drops
/// the token without consuming" path so we never leak.
enum DpdkRxToken {
    Mbuf {
        handle: *mut c_void,
        data: *const u8,
        len: usize,
    },
    Synth(Vec<u8>),
}

struct DpdkTxToken<'a> {
    dev: &'a mut DpdkDevice,
}

impl RxToken for DpdkRxToken {
    fn consume<R, F>(self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        // Suppress Drop so the mbuf/free below isn't run a second time.
        let mut this = core::mem::ManuallyDrop::new(self);
        match &mut *this {
            DpdkRxToken::Mbuf { handle, data, len } => {
                let slice = unsafe { core::slice::from_raw_parts(*data, *len) };
                let r = f(slice);
                unsafe { shim_mbuf_free(*handle) };
                r
            }
            DpdkRxToken::Synth(buf) => f(&buf[..]),
        }
    }
}

impl Drop for DpdkRxToken {
    fn drop(&mut self) {
        if let DpdkRxToken::Mbuf { handle, .. } = *self {
            unsafe { shim_mbuf_free(handle) };
        }
    }
}

impl<'a> TxToken for DpdkTxToken<'a> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let mut handle: *mut c_void = ptr::null_mut();
        let mut cap: u16 = 0;
        let data =
            unsafe { shim_mbuf_alloc_tx(self.dev.pool, &mut handle, &mut cap) };
        if data.is_null() || handle.is_null() {
            // Pool exhausted. smoltcp expects `f` to be called; discard
            // its output into a stack scratch and let it retransmit.
            let mut scratch = [0u8; MTU];
            let n = core::cmp::min(len, scratch.len());
            return f(&mut scratch[..n]);
        }
        let n = core::cmp::min(len, cap as usize);
        let slice = unsafe { core::slice::from_raw_parts_mut(data, n) };
        let r = f(slice);
        // shim_mbuf_tx consumes the mbuf on success and frees it on
        // tx_burst failure. Either way, `handle` must not be touched
        // after this call.
        let _ = unsafe { shim_mbuf_tx(self.dev.port_id, self.dev.queue_id, handle, n as u16) };
        r
    }
}

impl Device for DpdkDevice {
    type RxToken<'a>
        = DpdkRxToken
    where
        Self: 'a;
    type TxToken<'a>
        = DpdkTxToken<'a>
    where
        Self: 'a;

    fn receive(&mut self, _t: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        if let Some(buf) = self.pending_synth.take() {
            return Some((DpdkRxToken::Synth(buf), DpdkTxToken { dev: self }));
        }
        let mut handle: *mut c_void = ptr::null_mut();
        let mut data: *const u8 = ptr::null();
        let mut len: u16 = 0;
        let rc = unsafe {
            shim_mbuf_rx_burst(self.port_id, self.queue_id, &mut handle, &mut data, &mut len)
        };
        if rc != 1 || handle.is_null() {
            return None;
        }
        let rx = DpdkRxToken::Mbuf {
            handle,
            data,
            len: len as usize,
        };
        Some((rx, DpdkTxToken { dev: self }))
    }

    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        Some(DpdkTxToken { dev: self })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.max_transmission_unit = MTU;
        c.medium = Medium::Ethernet;
        // The ENA NIC is configured (via the shim's shim_eth_dev_configure)
        // to compute IPv4/TCP/UDP checksums on TX and verify them on RX;
        // the shim drops packets the NIC flags as bad. Ask smoltcp to stay
        // out of the checksum business entirely so we're not paying the CPU
        // cost twice.
        c.checksum = ChecksumCapabilities::ignored();
        c
    }
}

// Build a 42-byte ARP request frame asking who-has(target_ip). Sender is
// (our_mac, our_ip); target_hw is zeroed. Broadcast destination.
fn build_arp_request(our_mac: [u8; 6], our_ip: [u8; 4], target_ip: [u8; 4]) -> [u8; 42] {
    let mut f = [0u8; 42];
    // Ethernet header: dst=broadcast, src=our_mac, ethertype=0x0806 (ARP).
    f[0..6].fill(0xff);
    f[6..12].copy_from_slice(&our_mac);
    f[12..14].copy_from_slice(&[0x08, 0x06]);
    // ARP payload: HTYPE=1, PTYPE=0x0800, HLEN=6, PLEN=4, OPER=1 (request).
    f[14..16].copy_from_slice(&[0x00, 0x01]);
    f[16..18].copy_from_slice(&[0x08, 0x00]);
    f[18] = 6;
    f[19] = 4;
    f[20..22].copy_from_slice(&[0x00, 0x01]);
    f[22..28].copy_from_slice(&our_mac);
    f[28..32].copy_from_slice(&our_ip);
    // target_hw stays zero (that's what we want to learn).
    f[38..42].copy_from_slice(&target_ip);
    f
}

// Build the ARP-reply we'd expect if `sender_ip` answered our request.
// Ethernet dst = us, ethertype ARP, opcode 2.
fn build_arp_reply(
    sender_mac: [u8; 6],
    sender_ip: [u8; 4],
    target_mac: [u8; 6],
    target_ip: [u8; 4],
) -> Vec<u8> {
    let mut f = alloc::vec![0u8; 42];
    f[0..6].copy_from_slice(&target_mac);
    f[6..12].copy_from_slice(&sender_mac);
    f[12..14].copy_from_slice(&[0x08, 0x06]);
    f[14..16].copy_from_slice(&[0x00, 0x01]);
    f[16..18].copy_from_slice(&[0x08, 0x00]);
    f[18] = 6;
    f[19] = 4;
    f[20..22].copy_from_slice(&[0x00, 0x02]); // reply
    f[22..28].copy_from_slice(&sender_mac);
    f[28..32].copy_from_slice(&sender_ip);
    f[32..38].copy_from_slice(&target_mac);
    f[38..42].copy_from_slice(&target_ip);
    f
}

// If `frame` is an ARP reply announcing `expected_ip`, return the
// sender's MAC.
fn parse_arp_reply_from(frame: &[u8], expected_ip: [u8; 4]) -> Option<[u8; 6]> {
    if frame.len() < 42 || &frame[12..14] != &[0x08, 0x06] {
        return None;
    }
    if &frame[20..22] != &[0x00, 0x02] {
        return None;
    }
    if &frame[28..32] != &expected_ip[..] {
        return None;
    }
    let mut mac = [0u8; 6];
    mac.copy_from_slice(&frame[22..28]);
    Some(mac)
}

// =====================================================================
// DHCP + HTTPS GET to an S3 bucket in eu-north-1 hosting a 1 GiB
// bench.bin. Same-region S3 traffic goes over AWS's internal network
// (no inter-region hops, no internet egress cost), so this doubles as
// a receive-throughput benchmark: we time the download and report
// MB/s + Gbps at the end.
// =====================================================================

const TARGET_IP: Ipv4Address = Ipv4Address::new(3, 5, 216, 240);
const TARGET_PORT: u16 = 443;
const TARGET_SNI: &str = "miniosv-bench-1783870611.s3.eu-north-1.amazonaws.com";

// The URI + Host header hostname. Kept in the WorkerCtx-built request
// alongside the per-worker Range header.
const TARGET_HOST: &str = "miniosv-bench-1783870611.s3.eu-north-1.amazonaws.com";
const TARGET_PATH: &[u8] = b"/bench.bin";

/// Build a `GET /bench.bin HTTP/1.1 ... Range: bytes=A-B` request into
/// the caller-provided buffer, returning the number of bytes written.
/// Every worker splits the same file across a Range, so both workers
/// hit S3 for one file rather than duplicating the download.
fn build_range_request(buf: &mut [u8], start: u64, end_inclusive: u64) -> usize {
    use core::fmt::Write as _;
    struct Wr<'a> {
        buf: &'a mut [u8],
        used: usize,
    }
    impl<'a> core::fmt::Write for Wr<'a> {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            let n = core::cmp::min(s.len(), self.buf.len() - self.used);
            self.buf[self.used..self.used + n].copy_from_slice(&s.as_bytes()[..n]);
            self.used += n;
            Ok(())
        }
    }
    let mut w = Wr { buf, used: 0 };
    let _ = write!(
        &mut w,
        "GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: minidpdk-smoltcp/0.1\r\nRange: bytes={}-{}\r\nConnection: close\r\n\r\n",
        core::str::from_utf8(TARGET_PATH).unwrap_or("/"),
        TARGET_HOST,
        start,
        end_inclusive,
    );
    w.used
}

// Real monotonic clock: for smoltcp's poll timestamp we use elapsed ms
// since app start. Under a fake-tick clock (1 fake-ms per N iters) the
// interface's retransmit/backoff timers drift and stall at high
// throughput; a real clock keeps them accurate.
struct MonoClock {
    epoch_ns: u64,
}
impl MonoClock {
    fn new() -> Self {
        Self {
            epoch_ns: unsafe { shim_time_ns() },
        }
    }
    fn elapsed_ns(&self) -> u64 {
        unsafe { shim_time_ns() }.saturating_sub(self.epoch_ns)
    }
    fn elapsed_ms(&self) -> i64 {
        (self.elapsed_ns() / 1_000_000) as i64
    }
}

// ITER_BUDGET is a safety net so a hung run doesn't spin forever.
const ITER_BUDGET: u64 = 20_000_000_000;

/// Runs a DHCPv4 exchange until the server hands us an address, then
/// installs it (with default route + subnet mask) on the interface.
/// Returns the acquired (address, gateway) tuple.
fn dhcp_acquire(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    dhcp_handle: smoltcp::iface::SocketHandle,
    clk: &MonoClock,
) -> Option<(smoltcp::wire::Ipv4Cidr, Ipv4Address)> {
    let mut iter: u64 = 0;
    println!("DHCP: requesting lease...");
    loop {
        let now_ms = clk.elapsed_ms();
        iface.poll(Instant::from_millis(now_ms), dev, sockets);

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

        iter = iter.wrapping_add(1);
        if iter > ITER_BUDGET / 10 {
            println!("DHCP: timeout ({} iters, {} ms)", iter, now_ms);
            return None;
        }
    }
}

// ---------------------------------------------------------------------
// RDRAND-backed getrandom. Every RustCrypto dependency below rustls
// calls `getrandom::getrandom()` for ClientHello.random, ephemeral
// keys, and IVs. Its default backend is the Linux `getrandom(2)`
// syscall, which OSv doesn't provide. The `custom` cargo feature swaps
// that out for the callback registered below, and we back it with
// RDRAND — always present on the c5/c7 CPUs we deploy to.
// ---------------------------------------------------------------------

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

fn osv_getrandom(dest: &mut [u8]) -> Result<(), getrandom::Error> {
    let err = || getrandom::Error::from(core::num::NonZeroU32::new(1).unwrap());
    let mut i = 0;
    while i + 8 <= dest.len() {
        let v = rdrand64().ok_or_else(err)?;
        dest[i..i + 8].copy_from_slice(&v.to_ne_bytes());
        i += 8;
    }
    let remaining = dest.len() - i;
    if remaining > 0 {
        let v = rdrand64().ok_or_else(err)?;
        dest[i..].copy_from_slice(&v.to_ne_bytes()[..remaining]);
    }
    Ok(())
}

getrandom::register_custom_getrandom!(osv_getrandom);

// ---------------------------------------------------------------------
// TLS pump. Drives a rustls `UnbufferedClientConnection` state machine
// on top of the smoltcp TCP socket:
//
//   1. Drain any queued outgoing ciphertext into the TCP tx buffer.
//   2. Copy TCP rx bytes into `incoming` for rustls to consume.
//   3. Loop through `process_tls_records`: it hands us `EncodeTlsData`
//      (encode a handshake fragment), `TransmitTlsData` (nothing to
//      do — we already appended it), `WriteTraffic` (safe to write
//      plaintext request), `ReadTraffic` (records ready to decrypt),
//      or `BlockedHandshake` (need more incoming — wait a poll).
//   4. Exit when the peer sends FIN (CloseWait) and outgoing is empty.
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
// 16 KB + overhead. Sized to hold many pipelined records so `incoming`
// doesn't reallocate on every socket recv during the bulk download.
const TLS_BUF_CAP: usize = 256 * 1024;

fn https_get(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    tcp_handle: smoltcp::iface::SocketHandle,
    clk: &MonoClock,
    local_port: u16,
    request: &[u8],
) -> (usize, u64) {
    // --- TCP connect --------------------------------------------------
    {
        let s = sockets.get_mut::<tcp::Socket>(tcp_handle);
        if let Err(_) = s.connect(iface.context(), (TARGET_IP, TARGET_PORT), local_port) {
            println!("FAIL: tcp connect() rejected");
            return (0, 0);
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
            return (0, 0);
        }
    };
    let cfg = make_client_config();
    let mut conn = match UnbufferedClientConnection::new(cfg, server_name) {
        Ok(c) => c,
        Err(e) => {
            println!("FAIL: tls UnbufferedClientConnection::new: {:?}", e);
            return (0, 0);
        }
    };

    // Unbuffered-API buffers: `incoming` accumulates ciphertext read
    // from TCP until rustls can decode complete records; `outgoing`
    // holds ciphertext rustls asked us to send until TCP tx queue
    // drains it.
    let mut incoming: Vec<u8> = Vec::with_capacity(TLS_BUF_CAP);
    let mut outgoing: Vec<u8> = Vec::with_capacity(TLS_BUF_CAP);
    let mut request_queued = false;
    let mut handshake_done = false;
    let mut bytes_received: usize = 0;
    let mut iter: u64 = 0;
    let mut last_tcp_state: tcp::State = tcp::State::Closed;
    let mut t_request_sent_ns: u64 = 0;
    let connect_start_ms = clk.elapsed_ms();

    loop {
        let now_ms = clk.elapsed_ms();
        iface.poll(Instant::from_millis(now_ms), dev, sockets);
        let s = sockets.get_mut::<tcp::Socket>(tcp_handle);

        let state = s.state();
        if state != last_tcp_state {
            println!("tcp state: {:?}", state);
            last_tcp_state = state;
        }
        // If the SYN never gets its ACK, the response is landing on
        // another worker's RSS queue. Bail so the caller can retry with
        // a different source port.
        if state == tcp::State::SynSent && now_ms - connect_start_ms > 3000 {
            s.abort();
            return (0, 0);
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
                    return (bytes_received, 0);
                }
            };
            match st {
                ConnectionState::ReadTraffic(mut rt) => {
                    // Just tally byte counts; the response body would
                    // otherwise scroll the shim offload summary off
                    // the AWS console tail on completion.
                    while let Some(rec) = rt.next_record() {
                        match rec {
                            Ok(rec) => bytes_received += rec.payload.len(),
                            Err(e) => {
                                println!("FAIL: tls record: {:?}", e);
                                return (bytes_received, 0);
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
                            return (bytes_received, 0);
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
                        outgoing.resize(head + request.len() + 128, 0);
                        let n = match wt.encrypt(request, &mut outgoing[head..]) {
                            Ok(n) => n,
                            Err(e) => {
                                println!("FAIL: tls encrypt request: {:?}", e);
                                return (bytes_received, 0);
                            }
                        };
                        outgoing.truncate(head + n);
                        println!(
                            "OK: encrypted {} bytes of HTTP request into {} bytes of ciphertext",
                            request.len(),
                            n
                        );
                        request_queued = true;
                        t_request_sent_ns = clk.elapsed_ns();
                        progress = true;
                    }
                }
                ConnectionState::PeerClosed | ConnectionState::Closed => {
                    // Peer sent close_notify; nothing to send back — we
                    // wait for the TCP layer to see the FIN and exit
                    // via the end-condition check below.
                }
                _ => {}
            }
            incoming.drain(..discard);
        }

        // 4) End condition: peer sent FIN (TCP -> CloseWait or Closed).
        //    `is_active()` stays true in CloseWait so we can't rely on it.
        let ended = matches!(
            state,
            tcp::State::Closed | tcp::State::CloseWait | tcp::State::TimeWait
        );
        if handshake_done && request_queued && ended && outgoing.is_empty() {
            let elapsed_ns = clk.elapsed_ns().saturating_sub(t_request_sent_ns);
            let elapsed_s = elapsed_ns as f64 / 1e9;
            println!(
                "worker@q{}: HTTPS complete — {} B in {:.3} s",
                dev.queue_id, bytes_received, elapsed_s
            );
            return (bytes_received, elapsed_ns);
        }

        iter = iter.wrapping_add(1);
        if iter > ITER_BUDGET {
            println!(
                "worker@q{}: TIMEOUT after {} iters ({} ms, {} bytes)",
                dev.queue_id, iter, now_ms, bytes_received
            );
            return (bytes_received, clk.elapsed_ns().saturating_sub(t_request_sent_ns));
        }
    }
}

/// Per-thread state. `net` is the pre-learned network config (DHCP
/// happens once on the main thread; workers inherit it). Everything
/// else — Interface, sockets, rustls session — is thread-local inside
/// `worker_thread`.
#[repr(C)]
struct WorkerCtx {
    port_id: u16,
    queue_id: u16,
    local_port: u16,
    pool: *mut rte_pktmbuf_pool,
    mac: [u8; 6],
    ip: [u8; 4],
    prefix_len: u8,
    gateway_ip: [u8; 4],
    gateway_mac: [u8; 6],
    // Preformatted HTTP GET (with a `Range:` header) for this worker's
    // half of the file. `request_len` is the number of valid bytes.
    request: [u8; 384],
    request_len: u16,
    bytes_received: core::sync::atomic::AtomicU64,
    elapsed_ns: core::sync::atomic::AtomicU64,
}
unsafe impl Send for WorkerCtx {}
unsafe impl Sync for WorkerCtx {}

extern "C" fn worker_thread(arg: *mut c_void) {
    let ctx: &WorkerCtx = unsafe { &*(arg as *const WorkerCtx) };
    let clk = MonoClock::new();
    let ip = Ipv4Address::new(ctx.ip[0], ctx.ip[1], ctx.ip[2], ctx.ip[3]);
    let gw = Ipv4Address::new(
        ctx.gateway_ip[0],
        ctx.gateway_ip[1],
        ctx.gateway_ip[2],
        ctx.gateway_ip[3],
    );
    let mut dev = DpdkDevice {
        port_id: ctx.port_id,
        queue_id: ctx.queue_id,
        pool: ctx.pool,
        // Seed smoltcp's neighbor cache with the gateway MAC by handing
        // it a fabricated ARP reply on the first poll. This avoids each
        // worker doing its own ARP exchange, which RSS would misroute
        // (ARP replies land on queue 0).
        pending_synth: Some(build_arp_reply(
            ctx.gateway_mac,
            gw.octets(),
            ctx.mac,
            ip.octets(),
        )),
    };
    let config = Config::new(EthernetAddress(ctx.mac).into());
    let mut iface = Interface::new(config, &mut dev, Instant::from_millis(clk.elapsed_ms()));

    // Install the pre-learned IP + default route.
    iface.update_ip_addrs(|addrs| {
        let _ = addrs.push(IpCidr::new(ip.into(), ctx.prefix_len));
    });
    let _ = iface.routes_mut().add_default_ipv4_route(gw);

    // Per-thread TCP buffers (heap-allocated to keep the thread stack sane).
    let mut tcp_rx: Vec<u8> = alloc::vec![0u8; 4 * 1024 * 1024];
    let mut tcp_tx: Vec<u8> = alloc::vec![0u8; 16 * 1024];

    // Retry loop: RSS routes each 4-tuple to a specific queue by hashing
    // the return-flow tuple with a driver-picked key we can't observe. If
    // the SYN never gets an ACK, our port hashed to another worker's
    // queue; bump it and try again until we land on our own.
    let mut src_port = ctx.local_port;
    let mut bytes: usize = 0;
    let mut elapsed_ns: u64 = 0;
    for _attempt in 0..32u32 {
        let tcp_sock = tcp::Socket::new(
            tcp::SocketBuffer::new(&mut tcp_rx[..]),
            tcp::SocketBuffer::new(&mut tcp_tx[..]),
        );
        let mut storage: [SocketStorage<'_>; 1] = [SocketStorage::EMPTY];
        let mut sockets = SocketSet::new(&mut storage[..]);
        let tcp_handle = sockets.add(tcp_sock);
        let (b, e) = https_get(
            &mut iface,
            &mut dev,
            &mut sockets,
            tcp_handle,
            &clk,
            src_port,
            &ctx.request[..ctx.request_len as usize],
        );
        if b > 0 {
            bytes = b;
            elapsed_ns = e;
            break;
        }
        println!(
            "worker {}: SYN timed out on src_port {}, retrying",
            ctx.queue_id, src_port
        );
        src_port = src_port.wrapping_add(1);
    }
    ctx.bytes_received
        .store(bytes as u64, core::sync::atomic::Ordering::Relaxed);
    ctx.elapsed_ns
        .store(elapsed_ns, core::sync::atomic::Ordering::Relaxed);
}

/// Run on the main thread once, before spawning workers. Does DHCP on
/// queue 0 and then triggers an ARP for the gateway so we can extract
/// its MAC and hand it to the workers.
fn learn_network(
    pool: *mut rte_pktmbuf_pool,
    port_id: u16,
    mac: [u8; 6],
) -> Option<(Ipv4Address, u8, Ipv4Address, EthernetAddress)> {
    let clk = MonoClock::new();
    // Scope the smoltcp iface to the DHCP phase — after DHCP we use raw
    // shim RX/TX to do the gateway ARP, so we don't need to keep smoltcp
    // holding a mut ref to `dev`.
    let (ip, prefix, gw) = {
        let mut dev = DpdkDevice {
            port_id,
            queue_id: 0,
            pool,
            pending_synth: None,
        };
        let config = Config::new(EthernetAddress(mac).into());
        let mut iface =
            Interface::new(config, &mut dev, Instant::from_millis(clk.elapsed_ms()));

        static mut STORAGE: [SocketStorage; 1] = [SocketStorage::EMPTY];
        let mut sockets = unsafe { SocketSet::new(&mut STORAGE[..]) };
        let dhcp_handle = sockets.add(dhcpv4::Socket::new());

        let (cidr, gw) =
            dhcp_acquire(&mut iface, &mut dev, &mut sockets, dhcp_handle, &clk)?;
        (cidr.address(), cidr.prefix_len(), gw)
    };

    // Raw ARP request for the gateway on queue 0. smoltcp normally handles
    // this via its neighbor cache, but per-worker ifaces (on non-0 queues)
    // can't reach the reply via RSS, so we resolve it once here and hand
    // the MAC out to workers, who prime their own caches with a synthetic
    // ARP reply (see `build_arp_reply` + `pending_synth`).
    let req = build_arp_request(mac, ip.octets(), gw.octets());
    unsafe {
        let mut handle: *mut c_void = ptr::null_mut();
        let mut cap: u16 = 0;
        let data = shim_mbuf_alloc_tx(pool, &mut handle, &mut cap);
        if data.is_null() || handle.is_null() {
            println!("FAIL: no mbuf for ARP request");
            return None;
        }
        let n = core::cmp::min(req.len(), cap as usize);
        core::ptr::copy_nonoverlapping(req.as_ptr(), data, n);
        let _ = shim_mbuf_tx(port_id, 0, handle, n as u16);
    }

    let mut iter: u64 = 0;
    loop {
        let mut handle: *mut c_void = ptr::null_mut();
        let mut data: *const u8 = ptr::null();
        let mut len: u16 = 0;
        let rc =
            unsafe { shim_mbuf_rx_burst(port_id, 0, &mut handle, &mut data, &mut len) };
        if rc == 1 && !handle.is_null() {
            let slice = unsafe { core::slice::from_raw_parts(data, len as usize) };
            let hw = parse_arp_reply_from(slice, gw.octets());
            unsafe { shim_mbuf_free(handle) };
            if let Some(hw) = hw {
                println!(
                    "gateway MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]
                );
                return Some((ip, prefix, gw, EthernetAddress(hw)));
            }
        }
        iter = iter.wrapping_add(1);
        if iter > ITER_BUDGET / 20 {
            println!("FAIL: gateway ARP timed out");
            return None;
        }
    }
}

// =====================================================================
// Entry point
// =====================================================================

use core::sync::atomic::{AtomicU64, Ordering};

#[unsafe(no_mangle)]
pub extern "C" fn osv_app_main() {
    const N: u16 = 2;
    const N_QUEUES: u16 = N;
    let mut opened: Option<(PktPool, [u8; 6], u16)> = None;
    for port_id in 0u16..64 {
        println!("Probing port {}...", port_id);
        if let Some((pool, mac)) = probe_and_open(port_id, N_QUEUES) {
            opened = Some((pool, mac, port_id));
            break;
        }
    }
    let (pool, mac, port_id) = match opened {
        Some(x) => x,
        None => {
            println!("FAIL: no usable NIC found");
            loop {
                core::hint::spin_loop();
            }
        }
    };

    // Main-thread network setup: DHCP + gateway ARP on queue 0. Workers
    // inherit the resulting (ip, prefix, gateway_ip, gateway_mac) so
    // they never have to do their own DHCP or ARP (both of which would
    // race against RSS routing).
    let (ip, prefix_len, gw, gw_mac) = match learn_network(pool.0, port_id, mac) {
        Some(x) => x,
        None => loop {
            core::hint::spin_loop();
        },
    };
    let ip_bytes = ip.octets();
    let gw_bytes = gw.octets();

    // Split the file across N workers via HTTP Range. Each worker
    // fetches a distinct byte range from bench.bin, so total download
    // bytes = FILE_SIZE (not N * FILE_SIZE). Adjust FILE_SIZE if you
    // upload a different bench.bin.
    const FILE_SIZE: u64 = 1_073_741_824; // 1 GiB
    let chunk = FILE_SIZE / (N as u64);

    fn build_ctx(
        port_id: u16,
        queue_id: u16,
        local_port: u16,
        pool: *mut rte_pktmbuf_pool,
        mac: [u8; 6],
        ip: [u8; 4],
        prefix_len: u8,
        gateway_ip: [u8; 4],
        gateway_mac: [u8; 6],
        start: u64,
        end_inclusive: u64,
    ) -> alloc::boxed::Box<WorkerCtx> {
        let mut ctx = alloc::boxed::Box::new(WorkerCtx {
            port_id,
            queue_id,
            local_port,
            pool,
            mac,
            ip,
            prefix_len,
            gateway_ip,
            gateway_mac,
            request: [0u8; 384],
            request_len: 0,
            bytes_received: AtomicU64::new(0),
            elapsed_ns: AtomicU64::new(0),
        });
        let n = build_range_request(&mut ctx.request, start, end_inclusive);
        ctx.request_len = n as u16;
        ctx
    }

    // Each worker starts probing from a well-spread source port so it's
    // very unlikely all initial ports hash to the same RSS queue. If a
    // worker's return flow lands on someone else's queue the connect
    // stalls in SynSent; the worker then retries with local_port+N.
    let mut ctxs: alloc::vec::Vec<alloc::boxed::Box<WorkerCtx>> = alloc::vec::Vec::new();
    for i in 0..N as u64 {
        let start = i * chunk;
        let end_inclusive = if i == (N as u64) - 1 { FILE_SIZE - 1 } else { start + chunk - 1 };
        let local_port = 49152 + (i as u16) * 1000;
        println!("worker {}: queue {} src_port {} bytes {}..{}", i, i, local_port, start, end_inclusive);
        ctxs.push(build_ctx(
            port_id,
            i as u16,
            local_port,
            pool.0, mac, ip_bytes, prefix_len, gw_bytes, gw_mac.0,
            start, end_inclusive,
        ));
    }

    let overall_clk = MonoClock::new();
    println!("spawning {} workers...", N);
    let mut handles: alloc::vec::Vec<*mut c_void> = alloc::vec::Vec::new();
    for (i, ctx) in ctxs.iter().enumerate() {
        let h = unsafe {
            shim_thread_spawn(
                worker_thread,
                (&**ctx as *const WorkerCtx) as *mut c_void,
                i as c_int,
            )
        };
        handles.push(h);
    }

    for h in handles {
        unsafe { shim_thread_join(h) };
    }
    let overall_ns = overall_clk.elapsed_ns();

    let mut total_b: u64 = 0;
    for (i, ctx) in ctxs.iter().enumerate() {
        let b = ctx.bytes_received.load(Ordering::Relaxed);
        let e = ctx.elapsed_ns.load(Ordering::Relaxed) as f64 / 1e9;
        total_b += b;
        println!(
            "worker {} (q{}): {} B / {:.3} s  ({:.1} MB/s)",
            i, i, b, e,
            (b as f64 / 1e6) / e.max(1e-9)
        );
    }
    let overall_s = overall_ns as f64 / 1e9;
    let mib = total_b as f64 / (1024.0 * 1024.0);
    let mbps = total_b as f64 / 1e6 / overall_s.max(1e-9);
    let gbps = total_b as f64 * 8.0 / 1e9 / overall_s.max(1e-9);

    println!();
    println!(
        "AGGREGATE: {:.1} MiB in {:.3} s => {:.1} MB/s, {:.3} Gbps",
        mib, overall_s, mbps, gbps
    );

    unsafe { shim_offload_report() };
    unsafe { shim_dev_stop(port_id) };
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
