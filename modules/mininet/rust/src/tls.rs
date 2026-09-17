//! rustls configuration: no_std, compiled-in Mozilla roots, ring (`--features rustcrypto` for the pure-Rust provider).

use alloc::sync::Arc;

use rustls::client::ClientConfig;
use rustls::pki_types::UnixTime;
use rustls::time_provider::TimeProvider;
use rustls::RootCertStore;

use crate::ffi::shim_time_seconds;

/// Certificate validity needs the wall clock.
#[derive(Debug)]
struct ShimTimeProvider;

impl TimeProvider for ShimTimeProvider {
    fn current_time(&self) -> Option<UnixTime> {
        Some(UnixTime::since_unix_epoch(core::time::Duration::from_secs(
            unsafe { shim_time_seconds() },
        )))
    }
}

#[cfg(not(feature = "rustcrypto"))]
fn provider() -> rustls::crypto::CryptoProvider {
    rustls::crypto::ring::default_provider()
}

#[cfg(feature = "rustcrypto")]
fn provider() -> rustls::crypto::CryptoProvider {
    rustls_rustcrypto::provider()
}

/// Built once per worker: parsing the root bundle is not per-connection work.
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

pub(crate) const TLS_BUF_CAP: usize = 256 * 1024;
