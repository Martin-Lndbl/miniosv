//! Unit tests for the pure-logic parts, driven by test/os-mininet.cc (make app=test):
//! no_std, no `cargo test` target, and the guest's real allocator is where a bounds bug shows.

use alloc::vec;
use alloc::vec::Vec;

use crate::http::{BodySink, BufferSink, ContentRange, ParseError, ResponseParser};

/// Accumulates failures so one run reports every broken case.
pub struct Report {
    pub failures: u32,
    pub checks: u32,
    verbose: bool,
}

impl Report {
    fn new(verbose: bool) -> Self {
        Self { failures: 0, checks: 0, verbose }
    }

    fn check(&mut self, ok: bool, what: &str) -> bool {
        self.checks += 1;
        if !ok {
            self.failures += 1;
            println!("  FAIL {}", what);
        } else if self.verbose {
            println!("  ok   {}", what);
        }
        ok
    }

    fn eq_u64(&mut self, got: u64, want: u64, what: &str) -> bool {
        let ok = got == want;
        self.checks += 1;
        if !ok {
            self.failures += 1;
            println!("  FAIL {}: got {} want {}", what, got, want);
        } else if self.verbose {
            println!("  ok   {}", what);
        }
        ok
    }
}

/// A sink over a heap buffer with guard bytes, so an out-of-bounds write is detected.
struct Guarded {
    mem: Vec<u8>,
    off: usize,
    cap: usize,
}

const GUARD: usize = 64;
const GUARD_BYTE: u8 = 0xA5;

impl Guarded {
    fn new(cap: usize) -> Self {
        let mem = vec![GUARD_BYTE; cap + 2 * GUARD];
        Self { mem, off: GUARD, cap }
    }

    fn sink(&mut self) -> BufferSink {
        unsafe { BufferSink::new(self.mem.as_mut_ptr().add(self.off), self.cap) }
    }

    /// True when neither guard band was touched.
    fn intact(&self) -> bool {
        self.mem[..self.off].iter().all(|&b| b == GUARD_BYTE)
            && self.mem[self.off + self.cap..].iter().all(|&b| b == GUARD_BYTE)
    }

    fn body(&self) -> &[u8] {
        &self.mem[self.off..self.off + self.cap]
    }
}

fn test_buffer_sink(r: &mut Report) {
    println!("-- BufferSink");

    // Exact fit: everything stored, nothing overflowed, guards untouched.
    {
        let mut g = Guarded::new(8);
        {
            let mut s = g.sink();
            s.write(b"abcdefgh");
            r.eq_u64(BodySink::written(&s), 8, "exact fit stores all 8");
            r.check(!BodySink::overflowed(&s), "exact fit does not flag overflow");
        }
        r.check(g.intact(), "exact fit leaves guard bands intact");
        r.check(g.body() == b"abcdefgh", "exact fit stores the right bytes");
    }

    // One byte too many: the excess is dropped, not written past the end.
    {
        let mut g = Guarded::new(4);
        {
            let mut s = g.sink();
            s.write(b"abcde");
            r.eq_u64(BodySink::written(&s), 4, "overlong write stores only cap");
            r.check(BodySink::overflowed(&s), "overlong write flags overflow");
        }
        r.check(g.intact(), "overlong write does not touch guard bands");
        r.check(g.body() == b"abcd", "overlong write keeps the first cap bytes");
    }

    // Many small writes that cross cap partway through one of them.
    {
        let mut g = Guarded::new(10);
        {
            let mut s = g.sink();
            for _ in 0..5 {
                s.write(b"xyz"); // 15 bytes offered into a 10-byte buffer
            }
            r.eq_u64(BodySink::written(&s), 10, "repeated writes stop at cap");
            r.check(BodySink::overflowed(&s), "repeated writes flag overflow");
        }
        r.check(g.intact(), "repeated writes do not touch guard bands");
    }

    // Writes after full must be no-ops: `cap - written` is unsigned.
    {
        let mut g = Guarded::new(3);
        {
            let mut s = g.sink();
            s.write(b"abc");
            s.write(b"defghijklmnop");
            s.write(b"q");
            r.eq_u64(BodySink::written(&s), 3, "writes past full stay at cap");
        }
        r.check(g.intact(), "writes past full do not touch guard bands");
    }

    // A zero-capacity sink (the HEAD path) must absorb writes harmlessly.
    {
        let mut g = Guarded::new(0);
        {
            let mut s = g.sink();
            s.write(b"anything");
            r.eq_u64(BodySink::written(&s), 0, "zero-cap sink stores nothing");
            r.check(BodySink::overflowed(&s), "zero-cap sink flags overflow");
        }
        r.check(g.intact(), "zero-cap sink does not touch guard bands");
    }

    // An empty write must not move the cursor or flag overflow.
    {
        let mut g = Guarded::new(4);
        let mut s = g.sink();
        s.write(b"");
        r.eq_u64(BodySink::written(&s), 0, "empty write stores nothing");
        r.check(!BodySink::overflowed(&s), "empty write does not flag overflow");
    }
}

/// Feed a whole response through the parser in one call.
fn parse_all(bytes: &[u8], sink: &mut dyn BodySink) -> (ResponseParser, Result<(), ParseError>) {
    let mut p = ResponseParser::new();
    let rc = p.feed(bytes, sink);
    (p, rc)
}

fn test_response_parser(r: &mut Report) {
    println!("-- ResponseParser");

    // The ordinary case: a 206 with a body in the same buffer as the head.
    {
        let mut g = Guarded::new(5);
        let mut s = g.sink();
        let (p, rc) = parse_all(
            b"HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\n\
              Content-Range: bytes 0-4/100\r\nETag: \"abc\"\r\n\r\nhello",
            &mut s,
        );
        r.check(rc.is_ok(), "206 parses");
        r.eq_u64(p.status() as u64, 206, "206 status");
        r.eq_u64(p.body_bytes(), 5, "206 body_bytes counts body only, not head");
        r.eq_u64(BodySink::written(&s), 5, "206 body reaches the sink");
        r.check(p.headers_done(), "206 marks headers done");
        r.check(p.head().content_length == Some(5), "206 content-length");
        r.check(
            p.head().content_range == Some(ContentRange { first: 0, last: 4, total: Some(100) }),
            "206 content-range",
        );
        r.check(p.head().etag.as_deref() == Some("\"abc\""), "206 etag");
    }

    // The CRLFCRLF terminator split across feeds.
    for split in 1..46usize {
        let msg = b"HTTP/1.1 206 Partial\r\nContent-Length: 3\r\n\r\nabc";
        if split >= msg.len() {
            break;
        }
        let mut g = Guarded::new(3);
        let mut s = g.sink();
        let mut p = ResponseParser::new();
        let a = p.feed(&msg[..split], &mut s);
        let b = p.feed(&msg[split..], &mut s);
        let ok = a.is_ok()
            && b.is_ok()
            && p.status() == 206
            && p.body_bytes() == 3
            && BodySink::written(&s) == 3
            && g.intact();
        if !r.check(ok, "head split across two feeds parses identically") && split < 4 {
            println!("    (split at {})", split);
        }
    }

    // A body delivered in several records must accumulate, not reset.
    {
        let mut g = Guarded::new(9);
        let mut s = g.sink();
        let mut p = ResponseParser::new();
        let _ = p.feed(b"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nabc", &mut s);
        let _ = p.feed(b"def", &mut s);
        let _ = p.feed(b"ghi", &mut s);
        r.eq_u64(p.body_bytes(), 9, "body across three feeds accumulates");
        r.check(g.body() == b"abcdefghi", "body across three feeds is in order");
        r.check(g.intact(), "multi-feed body does not touch guard bands");
    }

    // More body than promised must not write past the buffer.
    {
        let mut g = Guarded::new(4);
        let mut s = g.sink();
        let (p, _) = parse_all(
            b"HTTP/1.1 206 Partial\r\nContent-Length: 4\r\n\r\nAAAABBBBCCCC",
            &mut s,
        );
        r.eq_u64(p.body_bytes(), 12, "over-long body is counted in full");
        r.eq_u64(BodySink::written(&s), 4, "over-long body is stored only up to cap");
        r.check(BodySink::overflowed(&s), "over-long body flags overflow");
        r.check(g.intact(), "over-long body does not touch guard bands");
    }

    // Chunked is rejected rather than guessed at: guessing truncates silently.
    {
        let mut sink = crate::http::NullSink;
        let (_, rc) = parse_all(
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
            &mut sink,
        );
        r.check(rc == Err(ParseError::Chunked), "chunked is rejected");
    }

    // Malformed status lines.
    for (bytes, what) in [
        (&b"nonsense\r\n\r\n"[..], "non-HTTP status line rejected"),
        (&b"HTTP/1.1 20 OK\r\n\r\n"[..], "two-digit status rejected"),
        (&b"HTTP/1.1 2O6 OK\r\n\r\n"[..], "non-digit status rejected"),
    ] {
        let mut sink = crate::http::NullSink;
        let (_, rc) = parse_all(bytes, &mut sink);
        r.check(rc == Err(ParseError::BadStatusLine), what);
    }

    // A head that never terminates must be bounded, not buffered forever.
    {
        let mut sink = crate::http::NullSink;
        let mut p = ResponseParser::new();
        let filler = vec![b'x'; 8 * 1024];
        let mut rc = Ok(());
        for _ in 0..16 {
            rc = p.feed(&filler, &mut sink);
            if rc.is_err() {
                break;
            }
        }
        r.check(rc == Err(ParseError::HeadTooLong), "endless head is bounded");
    }

    // Header parsing details that decide whether httpfs sees a range at all.
    {
        let mut sink = crate::http::NullSink;
        let (p, rc) = parse_all(
            b"HTTP/1.1 206 Partial\r\ncOnTeNt-LeNgTh: 7\r\n\
              Content-Range: bytes 10-16/*\r\n\r\nabcdefg",
            &mut sink,
        );
        r.check(rc.is_ok(), "case-insensitive header names parse");
        r.check(p.head().content_length == Some(7), "mixed-case content-length read");
        r.check(
            p.head().content_range == Some(ContentRange { first: 10, last: 16, total: None }),
            "content-range with * total",
        );
    }

    // 204 and 304 carry no body; the head is still parsed.
    {
        let mut sink = crate::http::NullSink;
        let (p, rc) = parse_all(b"HTTP/1.1 204 No Content\r\n\r\n", &mut sink);
        r.check(rc.is_ok(), "204 parses");
        r.eq_u64(p.body_bytes(), 0, "204 has no body");
        r.check(p.head().content_length.is_none(), "204 has no content-length");
    }
}

/// The arithmetic `Conn` uses to decide a response is finished.
fn test_completion_rule(r: &mut Report) {
    println!("-- completion rule");

    // Complete when body_bytes reaches content_length, and not before.
    let mut g = Guarded::new(6);
    let mut s = g.sink();
    let mut p = ResponseParser::new();
    let _ = p.feed(b"HTTP/1.1 206 Partial\r\nContent-Length: 6\r\n\r\nabc", &mut s);
    let want = p.head().content_length.unwrap_or(0);
    r.check(p.body_bytes() < want, "half a body is not complete");
    let _ = p.feed(b"def", &mut s);
    r.check(p.body_bytes() >= want, "a full body is complete");
    r.check(g.intact(), "completion path does not touch guard bands");
}

/// A connection that ends without a response is a failure, not an empty success.
fn test_close_without_response(r: &mut Report) {
    println!("-- close without a response");

    // Nothing at all arrived.
    {
        let mut p = ResponseParser::new();
        r.check(!p.headers_done(), "a parser fed nothing has no head");
        r.eq_u64(p.status() as u64, 0, "no head means status 0");
        r.eq_u64(p.body_bytes(), 0, "no head means no body");
        let mut sink = crate::http::NullSink;
        let _ = p.feed(b"", &mut sink);
        r.check(!p.headers_done(), "an empty feed still has no head");
    }

    // A head cut off mid-way is not a head.
    {
        let mut sink = crate::http::NullSink;
        let mut p = ResponseParser::new();
        let rc = p.feed(b"HTTP/1.1 206 Partial\r\nContent-Length: 5\r\n", &mut sink);
        r.check(rc.is_ok(), "a partial head parses so far without error");
        r.check(!p.headers_done(), "a partial head is not done");
        r.eq_u64(p.status() as u64, 0, "a partial head has no status yet");
    }

    // A 204 has a head, so a close after it is a real completion.
    {
        let mut sink = crate::http::NullSink;
        let mut p = ResponseParser::new();
        let _ = p.feed(b"HTTP/1.1 204 No Content\r\n\r\n", &mut sink);
        r.check(p.headers_done(), "a bodyless response still finishes its head");
        r.eq_u64(p.status() as u64, 204, "204 status is readable");
    }
}

fn test_queue(r: &mut Report) {
    let (got, dups, total) = crate::service::queue_selftest();
    r.eq_u64(got, total, "lock-free queue delivers every push");
    r.eq_u64(dups, 0, "lock-free queue delivers each push once");
}

/// Runs every check. Returns the number that failed.
pub fn run(verbose: bool) -> u32 {
    let mut r = Report::new(verbose);
    test_buffer_sink(&mut r);
    test_response_parser(&mut r);
    test_completion_rule(&mut r);
    test_close_without_response(&mut r);
    test_queue(&mut r);
    println!(
        "mininet selftest: {} checks, {} failed",
        r.checks, r.failures
    );
    r.failures
}
