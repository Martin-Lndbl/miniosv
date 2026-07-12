#![no_std]
#![allow(non_camel_case_types, dead_code)]

use core::ffi::c_int;
use core::fmt::{self, Write};
use core::panic::PanicInfo;

use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::{dhcpv4, tcp};
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpCidr, Ipv4Address};

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
// DHCP + HTTP GET against the AWS instance metadata service.
//
// The IP/gateway/subnet we get from AWS's DHCP server. The target is
// 169.254.169.254:80 (IMDSv1), which is a well-known link-local IP
// reachable from every VPC instance without any security-group
// changes. We ask for /latest/meta-data/instance-id — a short
// plaintext body that proves TCP-connect → HTTP request → response →
// close all worked end to end.
// =====================================================================

const TARGET_IP: Ipv4Address = Ipv4Address::new(169, 254, 169, 254);
const TARGET_PORT: u16 = 80;
const LOCAL_PORT: u16 = 49152;

const REQUEST: &[u8] = b"GET /latest/meta-data/instance-id HTTP/1.0\r\n\
                         Host: 169.254.169.254\r\n\
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

fn http_get(
    iface: &mut Interface,
    dev: &mut DpdkDevice,
    sockets: &mut SocketSet<'_>,
    tcp_handle: smoltcp::iface::SocketHandle,
    port_id: u16,
) {
    {
        let s = sockets.get_mut::<tcp::Socket>(tcp_handle);
        if let Err(_) = s.connect(iface.context(), (TARGET_IP, TARGET_PORT), LOCAL_PORT) {
            println!("FAIL: tcp connect() rejected");
            return;
        }
    }
    let o = TARGET_IP.octets();
    println!(
        "connecting to {}.{}.{}.{}:{} ...",
        o[0], o[1], o[2], o[3], TARGET_PORT
    );

    let mut request_sent = false;
    let mut bytes_received: usize = 0;
    let mut clock_ms: i64 = 0;
    let mut iter: u64 = 0;
    let mut last_state: tcp::State = tcp::State::Closed;

    loop {
        iface.poll(Instant::from_millis(clock_ms), dev, sockets);
        let s = sockets.get_mut::<tcp::Socket>(tcp_handle);

        let state = s.state();
        if state != last_state {
            println!("tcp state: {:?}", state);
            last_state = state;
        }

        if !request_sent && s.can_send() {
            match s.send_slice(REQUEST) {
                Ok(n) if n == REQUEST.len() => {
                    println!("OK: sent {} bytes of HTTP request", n);
                    request_sent = true;
                }
                Ok(n) => println!("PARTIAL: queued {}/{} bytes", n, REQUEST.len()),
                Err(_) => {
                    println!("FAIL: send_slice error");
                    return;
                }
            }
        }

        if s.can_recv() {
            let _ = s.recv(|buf| {
                bytes_received += buf.len();
                match core::str::from_utf8(buf) {
                    Ok(txt) => print!("{}", txt),
                    Err(_) => print!("<{} non-utf8 bytes>", buf.len()),
                }
                (buf.len(), ())
            });
        }

        if request_sent && !s.is_active() {
            println!();
            println!(
                "OK: HTTP exchange complete ({} bytes received)",
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

    http_get(&mut iface, &mut dev, &mut sockets, tcp_handle, port_id);
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
