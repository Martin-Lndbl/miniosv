#![no_std]

use core::panic::PanicInfo;
use smoltcp::iface::{Config, Interface, SocketSet};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{EthernetAddress, IpAddress, IpCidr, Ipv4Address};

unsafe extern "C" {
    fn printf(fmt: *const u8, ...) -> i32;
}

macro_rules! print {
    ($s:expr) => {
        unsafe { printf(concat!($s, "\0").as_ptr()); }
    };
}

const MTU: usize = 9000;
const QUEUE_SIZE: usize = 64;

struct StaticLoopback {
    queue: [[u8; MTU]; QUEUE_SIZE],
    lengths: [usize; QUEUE_SIZE],
    read_idx: usize,
    write_idx: usize,
    count: usize,
}

impl StaticLoopback {
    const fn new() -> Self {
        Self {
            queue: [[0u8; MTU]; QUEUE_SIZE],
            lengths: [0usize; QUEUE_SIZE],
            read_idx: 0,
            write_idx: 0,
            count: 0,
        }
    }
}

struct StaticRxToken<'a> {
    buffer: &'a [u8],
}

struct StaticTxToken<'a> {
    device: &'a mut StaticLoopback,
}

impl RxToken for StaticRxToken<'_> {
    fn consume<R, F>(self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        f(self.buffer)
    }
}

impl TxToken for StaticTxToken<'_> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let idx = self.device.write_idx;
        let result = f(&mut self.device.queue[idx][..len]);
        self.device.lengths[idx] = len;
        self.device.write_idx = (self.device.write_idx + 1) % QUEUE_SIZE;
        self.device.count += 1;
        result
    }
}

static mut RX_STAGING: [u8; MTU] = [0u8; MTU];

impl Device for StaticLoopback {
    type RxToken<'a> = StaticRxToken<'a> where Self: 'a;
    type TxToken<'a> = StaticTxToken<'a> where Self: 'a;

    fn receive(&mut self, _timestamp: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        if self.count == 0 {
            return None;
        }
        let rx_idx = self.read_idx;
        let len = self.lengths[rx_idx];

        unsafe {
            RX_STAGING[..len].copy_from_slice(&self.queue[rx_idx][..len]);
        }

        self.read_idx = (self.read_idx + 1) % QUEUE_SIZE;
        self.count -= 1;

        Some((
            StaticRxToken { buffer: unsafe { &RX_STAGING[..len] } },
            StaticTxToken { device: self },
        ))
    }

    fn transmit(&mut self, _timestamp: Instant) -> Option<Self::TxToken<'_>> {
        if self.count >= QUEUE_SIZE {
            return None;
        }
        Some(StaticTxToken { device: self })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut caps = DeviceCapabilities::default();
        caps.max_transmission_unit = MTU;
        caps.medium = Medium::Ethernet;
        caps
    }
}

static mut DEVICE: StaticLoopback = StaticLoopback::new();

#[unsafe(no_mangle)]
pub extern "C" fn osv_app_main() {
    print!("Starting smoltcp loopback benchmark...\n");

    let device = unsafe { &mut DEVICE };
    let config = Config::new(EthernetAddress([0x02, 0x00, 0x00, 0x00, 0x00, 0x01]).into());
    let mut iface = Interface::new(config, device, Instant::from_millis(0));
    iface.update_ip_addrs(|addrs| {
        addrs.push(IpCidr::new(IpAddress::Ipv4(Ipv4Address::new(127, 0, 0, 1)), 8)).unwrap();
    });

    static mut RX_BUF_S: [u8; 65536] = [0u8; 65536];
    static mut TX_BUF_S: [u8; 65536] = [0u8; 65536];
    static mut RX_BUF_C: [u8; 65536] = [0u8; 65536];
    static mut TX_BUF_C: [u8; 65536] = [0u8; 65536];
    static mut DATA:     [u8; 65536] = [0x2au8; 65536];

    let server_socket = unsafe {
        tcp::Socket::new(
            tcp::SocketBuffer::new(&mut RX_BUF_S[..]),
            tcp::SocketBuffer::new(&mut TX_BUF_S[..]),
        )
    };
    let client_socket = unsafe {
        tcp::Socket::new(
            tcp::SocketBuffer::new(&mut RX_BUF_C[..]),
            tcp::SocketBuffer::new(&mut TX_BUF_C[..]),
        )
    };

    static mut SOCKET_STORAGE: [smoltcp::iface::SocketStorage; 2] =
        [smoltcp::iface::SocketStorage::EMPTY; 2];

    let mut sockets = unsafe { SocketSet::new(&mut SOCKET_STORAGE[..]) };
    let server_handle = sockets.add(server_socket);
    let client_handle = sockets.add(client_socket);

    sockets.get_mut::<tcp::Socket>(server_handle).listen(1234).unwrap();
    sockets.get_mut::<tcp::Socket>(client_handle)
        .connect(iface.context(), (Ipv4Address::new(127, 0, 0, 1), 1234), 49152).unwrap();

    const TOTAL_BYTES: usize = 100 * 1024 * 1024 * 1024;
    let mut sent = 0usize;
    let mut received = 0usize;
    let mut tick = 0i64;

    print!("Transferring 100GB over loopback...\n");

    loop {
        let device = unsafe { &mut DEVICE };
        iface.poll(Instant::from_millis(tick), device, &mut sockets);
        tick += 1;

        // Drain receiver completely
        let server = sockets.get_mut::<tcp::Socket>(server_handle);
        while server.can_recv() {
            let n = server.recv(|buf| {
                let len = buf.len();
                (len, len)
            }).unwrap();
            received += n;
        }

        // Fill sender completely
        let client = sockets.get_mut::<tcp::Socket>(client_handle);
        while client.can_send() && sent < TOTAL_BYTES {
            let to_send = (TOTAL_BYTES - sent).min(65536);
            let n = unsafe { client.send_slice(&DATA[..to_send]).unwrap_or(0) };
            if n == 0 { break; }
            sent += n;
        }

        if received >= TOTAL_BYTES {
            break;
        }
    }

    print!("Done! Transferred 100GB over loopback.\n");
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
