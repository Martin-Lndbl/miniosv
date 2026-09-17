//! A smoltcp `Device` over one minidpdk queue. Nothing is copied on either path.

use alloc::vec::Vec;
use core::ffi::c_void;
use core::ptr;

use smoltcp::phy::{ChecksumCapabilities, Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::time::Instant;

use crate::ffi::{rte_pktmbuf_pool, shim_mbuf_alloc_tx, shim_mbuf_free, shim_mbuf_rx_burst_n, shim_mbuf_tx, PORT};
use crate::rss::OwnedPorts;
use crate::stats;

pub(crate) const MTU: usize = 1514;

/// RX drains up to this many mbufs per burst.
pub(crate) const RX_BURST: usize = 32;

pub(crate) struct DpdkDevice {
    pub(crate) queue_id: u16,
    pub(crate) pool: *mut rte_pktmbuf_pool,
    /// One fabricated frame, seeding the neighbour cache with the gateway's MAC.
    pub(crate) pending_synth: Option<Vec<u8>>,
    /// Ports this queue owns; `None` accepts everything (the DHCP/ARP phase on queue 0).
    /// A TCP frame for a port not owned is a prediction failure and is counted.
    pub(crate) owned_ports: Option<OwnedPorts>,
    rx_pref_handles: [*mut c_void; RX_BURST],
    rx_pref_data: [*const u8; RX_BURST],
    rx_pref_lens: [u16; RX_BURST],
    rx_pref_pos: u16,
    rx_pref_len: u16,
}

impl DpdkDevice {
    pub(crate) fn new(
        queue_id: u16,
        pool: *mut rte_pktmbuf_pool,
        owned_ports: Option<OwnedPorts>,
        pending_synth: Option<Vec<u8>>,
    ) -> Self {
        Self {
            queue_id,
            pool,
            pending_synth,
            owned_ports,
            rx_pref_handles: [ptr::null_mut(); RX_BURST],
            rx_pref_data: [ptr::null(); RX_BURST],
            rx_pref_lens: [0; RX_BURST],
            rx_pref_pos: 0,
            rx_pref_len: 0,
        }
    }

    /// True if the frame should be passed up.
    fn accepts(&self, bytes: &[u8]) -> bool {
        let owned = match &self.owned_ports {
            Some(o) => o,
            None => return true,
        };
        // Ethernet(14) + IPv4(20) + TCP(20) = 54.
        if bytes.len() < 54 {
            return true;
        }
        if bytes[12] != 0x08 || bytes[13] != 0x00 {
            return true; // not IPv4
        }
        let ihl = (bytes[14] & 0x0f) as usize * 4;
        let l4 = 14 + ihl;
        if bytes[23] != 6 /* TCP */ || l4 + 4 > bytes.len() {
            return true;
        }
        let dst_port = ((bytes[l4 + 2] as u16) << 8) | (bytes[l4 + 3] as u16);
        if owned.contains(dst_port) {
            true
        } else {
            stats::misrouted_drop();
            false
        }
    }
}

pub(crate) enum DpdkRxToken {
    Mbuf {
        handle: *mut c_void,
        data: *const u8,
        len: usize,
    },
    Synth(Vec<u8>),
}

pub(crate) struct DpdkTxToken<'a> {
    dev: &'a mut DpdkDevice,
}

impl RxToken for DpdkRxToken {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
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

impl TxToken for DpdkTxToken<'_> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        let mut handle: *mut c_void = ptr::null_mut();
        let mut cap: u16 = 0;
        let data = unsafe { shim_mbuf_alloc_tx(self.dev.pool, self.dev.queue_id, &mut handle, &mut cap) };
        if data.is_null() || handle.is_null() {
            // Pool exhausted: smoltcp still wants `f` called; count the discarded frame.
            stats::tx_alloc_fail();
            let mut scratch = [0u8; MTU];
            let n = core::cmp::min(len, scratch.len());
            return f(&mut scratch[..n]);
        }
        let n = core::cmp::min(len, cap as usize);
        let slice = unsafe { core::slice::from_raw_parts_mut(data, n) };
        let r = f(slice);
        if unsafe { shim_mbuf_tx(PORT, self.dev.queue_id, handle, n as u16) } != 0 {
            stats::tx_burst_fail();
        }
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
        loop {
            if self.rx_pref_pos == self.rx_pref_len {
                let got = unsafe {
                    shim_mbuf_rx_burst_n(
                        PORT,
                        self.queue_id,
                        self.rx_pref_handles.as_mut_ptr(),
                        self.rx_pref_data.as_mut_ptr(),
                        self.rx_pref_lens.as_mut_ptr(),
                        RX_BURST as u16,
                    )
                };
                if got == 0 {
                    return None;
                }
                self.rx_pref_pos = 0;
                self.rx_pref_len = got;
            }
            let i = self.rx_pref_pos as usize;
            self.rx_pref_pos += 1;
            let handle = self.rx_pref_handles[i];
            let data = self.rx_pref_data[i];
            let len = self.rx_pref_lens[i] as usize;

            let ok = self.accepts(unsafe { core::slice::from_raw_parts(data, len) });
            if ok {
                return Some((DpdkRxToken::Mbuf { handle, data, len }, DpdkTxToken { dev: self }));
            }
            unsafe { shim_mbuf_free(handle) };
        }
    }

    fn transmit(&mut self, _t: Instant) -> Option<Self::TxToken<'_>> {
        Some(DpdkTxToken { dev: self })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.max_transmission_unit = MTU;
        c.medium = Medium::Ethernet;
        // The NIC handles L3/L4 checksums; don't pay the CPU cost twice.
        c.checksum = ChecksumCapabilities::ignored();
        c
    }
}
