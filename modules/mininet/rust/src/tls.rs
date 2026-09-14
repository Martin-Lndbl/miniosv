//! rustls configuration.
//!
//! `no_std`, and the trust anchors are the compiled-in Mozilla bundle rather
//! than a file on disk. The crypto provider is ring: it is `#![no_std]` as
//! well, and its AES-GCM is around six times the pure-Rust provider's -- see
//! the note in Cargo.toml for the measurement. `--features rustcrypto` goes
//! back to the pure-Rust one.

use alloc::sync::Arc;

use rustls::client::ClientConfig;
use rustls::pki_types::UnixTime;
use rustls::time_provider::TimeProvider;
use rustls::RootCertStore;

use crate::ffi::shim_time_seconds;

/// Certificate validity needs a wall clock, not a monotonic one. OSv's is
/// whatever the platform handed it at boot; if it is wrong, every handshake
/// fails as expired rather than silently accepting anything.
#[derive(Debug)]
struct ShimTimeProvider;

impl TimeProvider for ShimTimeProvider {
    fn current_time(&self) -> Option<UnixTime> {
        Some(UnixTime::since_unix_epoch(core::time::Duration::from_secs(
            unsafe { shim_time_seconds() },
        )))
    }
}

/// The provider the build selected.
#[cfg(not(feature = "rustcrypto"))]
fn provider() -> rustls::crypto::CryptoProvider {
    rustls::crypto::ring::default_provider()
}

#[cfg(feature = "rustcrypto")]
fn provider() -> rustls::crypto::CryptoProvider {
    rustls_rustcrypto::provider()
}

/// Built once per worker and shared by its connections: assembling the root
/// store parses the whole bundle, which is not something to do per connection.
pub(crate) fn client_config() -> Arc<ClientConfig> {
    let mut roots = RootCertStore::empty();
    roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    let cfg = ClientConfig::builder_with_details(
        Arc::new(provider()),
        Arc::new(ShimTimeProvider),
    )
    .with_safe_default_protocol_versions()
    .expect("rustls: default protocol versions")
    .with_root_certificates(roots)
    .with_no_client_auth();
    Arc::new(cfg)
}

/// Sized to hold pipelined records without reallocating during a download.
pub(crate) const TLS_BUF_CAP: usize = 256 * 1024;
