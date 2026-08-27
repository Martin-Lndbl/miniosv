//! Just enough HTTP/1.1 response parsing to separate the body from the head.
//!
//! Deliberately minimal. It finds the status code and the CRLFCRLF that ends
//! the head, and everything after that is body. It does *not* understand
//! chunked transfer-encoding: S3 answers a ranged GET with `206` and a
//! `Content-Length`, so nothing here needs it yet, and guessing would silently
//! truncate a transfer rather than fail one. See `PLAN_duckdb_net.md`.

/// Where a response body goes.
///
/// The benchmark only wants the count, which the parser keeps regardless, so
/// it sinks into [`NullSink`]. A caller that wants the bytes -- DuckDB reading
/// a range into a page -- supplies its own.
pub trait BodySink {
    fn write(&mut self, data: &[u8]);
}

/// Counts and discards. A ZST, so boxing one does not allocate.
pub struct NullSink;

impl BodySink for NullSink {
    fn write(&mut self, _data: &[u8]) {}
}

pub(crate) struct ResponseParser {
    headers_done: bool,
    /// How much of the CRLFCRLF terminator has been seen. Carried across calls
    /// because it can straddle two TLS records.
    hdr_state: u8,
    status: u16,
    status_pos: u8,
    body_bytes: u64,
}

impl ResponseParser {
    pub(crate) fn new() -> Self {
        Self {
            headers_done: false,
            hdr_state: 0,
            status: 0,
            status_pos: 0,
            body_bytes: 0,
        }
    }

    pub(crate) fn status(&self) -> u16 {
        self.status
    }

    /// Body bytes only. The head is application data as far as TLS is
    /// concerned, so counting raw decrypted length overcounts by one head per
    /// connection and makes a byte-exact check impossible.
    pub(crate) fn body_bytes(&self) -> u64 {
        self.body_bytes
    }

    pub(crate) fn headers_done(&self) -> bool {
        self.headers_done
    }

    pub(crate) fn feed(&mut self, data: &[u8], sink: &mut dyn BodySink) {
        if self.headers_done {
            self.body_bytes += data.len() as u64;
            sink.write(data);
            return;
        }
        for (i, &b) in data.iter().enumerate() {
            // "HTTP/1.1 206 ..." -- the code is bytes 9..12. An error body
            // counts as bytes, so a refusal otherwise looks like a shortfall.
            if self.status_pos < 12 {
                if self.status_pos >= 9 && b.is_ascii_digit() {
                    self.status = self.status * 10 + (b - b'0') as u16;
                }
                self.status_pos += 1;
            }
            self.hdr_state = match (self.hdr_state, b) {
                (0, b'\r') => 1,
                (1, b'\n') => 2,
                (2, b'\r') => 3,
                (3, b'\n') => 4,
                (_, b'\r') => 1,
                _ => 0,
            };
            if self.hdr_state == 4 {
                self.headers_done = true;
                let body = &data[i + 1..];
                self.body_bytes += body.len() as u64;
                sink.write(body);
                return;
            }
        }
    }

    /// Count bytes without looking at them. Used when the record layer is
    /// stubbed out: the payload is ciphertext, so there is no head to find and
    /// no body to separate.
    pub(crate) fn count_opaque(&mut self, n: usize) {
        self.body_bytes += n as u64;
    }
}
