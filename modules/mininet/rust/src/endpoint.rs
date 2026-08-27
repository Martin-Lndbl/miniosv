//! What to dial, and what to send once dialled.

use alloc::string::String;

/// A server, by address. There is no resolver in the guest, so the caller
/// supplies the address it already knows -- baked in at build time today, from
/// a host table or DNS later. `host` is both the `Host:` header and the TLS
/// server name, so it must be the name the certificate is issued for even
/// though it is not what gets dialled.
#[derive(Clone)]
pub struct Endpoint {
    pub ip: [u8; 4],
    pub port: u16,
    pub host: String,
    /// False dials plain HTTP, which isolates the network stack from the
    /// record layer. The two are not comparable measurements.
    pub tls: bool,
}

impl Endpoint {
    /// The conventional port for the scheme, so callers do not repeat it.
    pub fn new(ip: [u8; 4], host: impl Into<String>, tls: bool) -> Self {
        Self {
            ip,
            port: if tls { 443 } else { 80 },
            host: host.into(),
            tls,
        }
    }

    pub fn is_configured(&self) -> bool {
        self.ip != [0, 0, 0, 0]
    }
}

/// One request on one connection.
///
/// `head` is the complete request head -- request line, headers, and the blank
/// line -- rendered by the caller. The stack does not build HTTP; it carries
/// it. That keeps signing, range arithmetic and header policy where they
/// belong, which for DuckDB is inside httpfs.
///
/// There is no endpoint here: the worker that carries the request is already
/// bound to one, because its usable source ports depend on the peer's address.
pub struct Request<'a> {
    pub head: &'a [u8],
    /// Diagnostic: after the handshake, throw ciphertext away instead of
    /// decrypting it. Separates record-layer cost from network cost. The
    /// transfer is then unverifiable byte-for-byte and `body_bytes` counts
    /// ciphertext, including TLS and HTTP framing. Ignored without TLS, where
    /// there is no record layer to skip.
    pub discard_ciphertext: bool,
}
