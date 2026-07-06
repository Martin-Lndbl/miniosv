#![no_std]
#![allow(non_camel_case_types, dead_code)]

use core::ffi::{c_int, c_void};
use core::fmt::{self, Write};
use core::panic::PanicInfo;

// =====================================================================
// Minimal console output (no std::cout in no_std).
// Assumes a libc `write(2)` is linkable in the OSv environment, same
// as the original app implicitly relied on via libstdc++/iostream.
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
// minidpdk FFI surface
//
// These declarations mirror rust_app/shim/shim.h exactly. The shim
// (shim.cc) is the only thing that ever touches real minidpdk structs
// (rte_eth_dev_info, rte_eth_conf, rte_eth_rxconf, rte_eth_txconf,
// rte_eth_stats) and the C++-only rte_eth_dev virtual methods /
// eth_os::get_eth_for_port(). Only plain integers and an opaque pool
// pointer cross the FFI boundary, so there is no struct-layout risk
// here — if you change a signature, change shim.h/shim.cc to match.
// =====================================================================

/// Opaque handle to a minidpdk packet-mbuf pool. Never dereferenced on
/// the Rust side; only ever passed back into the shim.
#[repr(C)]
pub struct rte_pktmbuf_pool {
    _private: [u8; 0],
}

extern "C" {
    // Returns 1 if a device exists for `port_id`, else 0.
    fn shim_is_valid_port(port_id: u16) -> c_int;

    // Fills *max_rx_queues / *max_tx_queues. Returns 0 on success, -1
    // if the port doesn't exist.
    fn shim_get_dev_info(port_id: u16, max_rx_queues: *mut u16, max_tx_queues: *mut u16) -> c_int;

    // Wraps rte_pktmbuf_pool_create(); returns null on failure.
    fn shim_pktmbuf_pool_create(
        name: *const u8,
        n: u32,
        cache_size: u32,
        priv_size: u16,
        data_room_size: u16,
    ) -> *mut rte_pktmbuf_pool;
    fn shim_mempool_free(pool: *mut rte_pktmbuf_pool);

    // Wraps rte_eth_dev_configure() with a zero-initialized rte_eth_conf.
    fn shim_eth_dev_configure(port_id: u16, nb_rx_q: u16, nb_tx_q: u16) -> c_int;

    fn shim_adjust_nb_rx_tx_desc(port_id: u16, nb_rx_desc: *mut u16, nb_tx_desc: *mut u16);

    // Wraps rte_eth_rx_queue_setup() with a zero-initialized rte_eth_rxconf.
    fn shim_rx_queue_setup(
        port_id: u16,
        queue_id: u16,
        nb_desc: u16,
        mempool: *mut rte_pktmbuf_pool,
    ) -> c_int;
    // Wraps rte_eth_tx_queue_setup() with a zero-initialized rte_eth_txconf.
    fn shim_tx_queue_setup(port_id: u16, queue_id: u16, nb_desc: u16) -> c_int;

    fn shim_dev_start(port_id: u16) -> c_int;
    // No-op if the port doesn't exist.
    fn shim_dev_stop(port_id: u16);

    // Writes 6 bytes into addr_bytes.
    fn shim_macaddr_get(port_id: u16, addr_bytes: *mut u8);

    // Fills the four counters. Returns 0 on success, -1 if the port
    // doesn't exist.
    fn shim_get_stats(
        port_id: u16,
        ipackets: *mut u64,
        opackets: *mut u64,
        ibytes: *mut u64,
        obytes: *mut u64,
    ) -> c_int;
}

// errno values (Linux/glibc numbering — adjust if OSv's libc differs).
const ENODEV: c_int = 19;
const ENOMEM: c_int = 12;

// =====================================================================
// App types
// =====================================================================

#[derive(Clone, Copy)]
struct EtherAddr {
    addr_bytes: [u8; 6],
}

impl Default for EtherAddr {
    fn default() -> Self {
        EtherAddr { addr_bytes: [0; 6] }
    }
}

#[allow(dead_code)]
struct AppConfig {
    src: EtherAddr,
    dst: EtherAddr,
    sip: u32,
    dip: u32,
    l4port: u32,
    mtu: u32,
}

impl Default for AppConfig {
    fn default() -> Self {
        AppConfig {
            src: EtherAddr::default(),
            dst: EtherAddr::default(),
            sip: 0,
            dip: 0,
            l4port: 0,
            mtu: 128,
        }
    }
}

/// RAII wrapper around the packet pool, mirroring the C++ `pool_ptr`
/// (`unique_ptr<rte_pktmbuf_pool, decltype(&rte_mempool_free)>`).
struct PktPool(*mut rte_pktmbuf_pool);

impl Drop for PktPool {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { shim_mempool_free(self.0) };
        }
    }
}

struct PortInfo {
    port_id: u16,
    addr: EtherAddr,
    pool: Option<PktPool>,
}

impl Default for PortInfo {
    fn default() -> Self {
        PortInfo {
            port_id: 0,
            addr: EtherAddr::default(),
            pool: None,
        }
    }
}

// =====================================================================
// Probe logic
// =====================================================================

fn probe_port(info: &mut PortInfo) -> c_int {
    const DESC_NUM: u16 = 64;
    const MEMPOOL_CACHE_SIZE: u32 = 32;
    const POOL_SIZE: u32 = 128;
    const DATA_ROOM_SIZE: u16 = 1536;

    let valid = unsafe { shim_is_valid_port(info.port_id) };
    if valid == 0 {
        println!("FAIL: no device found for port {}", info.port_id);
        return ENODEV;
    }
    println!("OK: device found");

    let mut max_rx_queues: u16 = 0;
    let mut max_tx_queues: u16 = 0;
    if unsafe { shim_get_dev_info(info.port_id, &mut max_rx_queues, &mut max_tx_queues) } != 0 {
        println!("FAIL: could not read device info");
        return ENODEV;
    }
    println!("OK: device info retrieved");
    println!("  max rx queues:{}", max_rx_queues);
    println!("  max tx queues:{}", max_tx_queues);

    let pool_name = b"probe-pool\0";
    let pool = unsafe {
        shim_pktmbuf_pool_create(
            pool_name.as_ptr(),
            POOL_SIZE,
            MEMPOOL_CACHE_SIZE,
            0,
            DATA_ROOM_SIZE,
        )
    };
    if pool.is_null() {
        println!("FAIL: could not allocate packet pool");
        return ENOMEM;
    }
    info.pool = Some(PktPool(pool));
    println!("OK: packet pool allocated");

    if unsafe { shim_eth_dev_configure(info.port_id, 1, 1) } != 0 {
        println!("FAIL: device configure failed");
        return 1;
    }
    println!("OK: device configured (1 rx queue, 1 tx queue)");

    let mut rx_desc: u16 = DESC_NUM;
    let mut tx_desc: u16 = DESC_NUM;
    unsafe { shim_adjust_nb_rx_tx_desc(info.port_id, &mut rx_desc, &mut tx_desc) };

    let pool_ptr = info.pool.as_ref().unwrap().0;
    if unsafe { shim_rx_queue_setup(info.port_id, 0, rx_desc, pool_ptr) } != 0 {
        println!("FAIL: rx queue setup failed");
        return 1;
    }
    println!("OK: rx queue set up ({} descriptors)", rx_desc);

    if unsafe { shim_tx_queue_setup(info.port_id, 0, tx_desc) } != 0 {
        println!("FAIL: tx queue setup failed");
        return 1;
    }
    println!("OK: tx queue set up ({} descriptors)", tx_desc);

    if unsafe { shim_dev_start(info.port_id) } != 0 {
        println!("FAIL: device start failed");
        return 1;
    }
    println!("OK: device started");

    unsafe { shim_macaddr_get(info.port_id, info.addr.addr_bytes.as_mut_ptr()) };

    let (mut ipackets, mut opackets, mut ibytes, mut obytes) = (0u64, 0u64, 0u64, 0u64);
    unsafe {
        shim_get_stats(
            info.port_id,
            &mut ipackets,
            &mut opackets,
            &mut ibytes,
            &mut obytes,
        )
    };
    println!(
        "OK: stats readable (rx: {} pkts, {} bytes)",
        ipackets, ibytes
    );

    0
}

#[unsafe(no_mangle)]
pub extern "C" fn osv_app_main() {
    let mut info = PortInfo::default();
    for i in 0u16..64 {
        info.port_id = i;
        println!("Probing port {}...", info.port_id);
        let rc = probe_port(&mut info);
        if rc == 0 {
            println!("RESULT: NIC probe succeeded (code {})", rc);
            unsafe { shim_dev_stop(info.port_id) };
            break;
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
