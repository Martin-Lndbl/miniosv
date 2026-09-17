//! Why the stack, or a connection, failed; coarse on purpose.

use core::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    NoDevice,
    NoMemory,
    /// Key or table unreadable, or table not a power of two.
    RssUnavailable,
    DhcpTimeout,
    ArpTimeout,
    NoPorts,
    ConnectRejected,
    SynTimeout,
    Tls,
    /// Includes chunked transfer-encoding, which is not implemented.
    BadResponse,
    BufferTooSmall,
}

impl Error {
    pub fn as_str(&self) -> &'static str {
        match self {
            Error::NoDevice => "no usable NIC",
            Error::NoMemory => "mempool allocation failed",
            Error::RssUnavailable => "RSS key or indirection table unavailable",
            Error::DhcpTimeout => "DHCP timed out",
            Error::ArpTimeout => "gateway ARP timed out",
            Error::NoPorts => "no ephemeral port steers to this queue",
            Error::ConnectRejected => "connect() rejected by the socket",
            Error::SynTimeout => "SYN timeout: no SYN-ACK",
            Error::Tls => "TLS failure",
            Error::BadResponse => "malformed or unsupported HTTP response",
            Error::BufferTooSmall => "response body exceeded the caller's buffer",
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}
