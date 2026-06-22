#![no_std]

use core::panic::PanicInfo;
use smoltcp::wire::{EthernetAddress, IpAddress, IpCidr, Ipv4Address};

unsafe extern "C" {
    fn printf(fmt: *const u8, ...) -> i32;
}

macro_rules! print {
    ($s:expr) => {
        unsafe { printf(concat!($s, "\0").as_ptr()); }
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}

#[unsafe(no_mangle)]
pub extern "C" fn osv_app_main() {
    let addr = EthernetAddress([0x02, 0x00, 0x00, 0x00, 0x00, 0x01]);
    let ip = IpCidr::new(IpAddress::Ipv4(Ipv4Address::new(192, 168, 1, 1)), 24);

    print!("MAC and IP types constructed successfully.\n");
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
