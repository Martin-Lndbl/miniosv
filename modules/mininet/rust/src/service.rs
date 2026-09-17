//! Requests submitted from other threads, answered by the workers: one
//! polling thread per worker, and a blocking [`Service::get`] for everyone
//! else. The ceiling is `workers * conns_per_worker` requests in flight.

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};

use crate::endpoint::{Endpoint, Request};
use crate::error::Error;
use crate::ffi::{shim_thread_current, shim_thread_park, shim_thread_unpark, shim_time_ns};
use crate::http::{BufferSink, ContentRange};
use crate::stats;
use crate::thread;
use crate::worker::{Worker, WorkerConfig, WorkerHandle};
use crate::Stack;

/// What a completed request delivered.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GetResult {
    pub status: u16,
    /// Bytes written into the caller's buffer.
    pub written: u64,
    /// What the head claimed the body was, when it said.
    pub content_length: Option<u64>,
    pub content_range: Option<ContentRange>,
    pub etag: Option<String>,
    pub last_modified: Option<String>,
}

/// Slot lifecycle. The submitter parks until this leaves `PENDING`, then waits
/// for `RELEASED` before returning -- see [`Slot`].
const PENDING: u32 = 0;
const PUBLISHED: u32 = 1;
const RELEASED: u32 = 2;

/// One request in flight, on the submitting thread's stack. The worker marks
/// `PUBLISHED`, wakes the submitter, then marks `RELEASED`; the submitter
/// returns only on `RELEASED`, so the worker never unparks a dead thread.
struct Slot {
    head: *const u8,
    head_len: usize,
    buf: *mut u8,
    buf_cap: usize,
    discard_ciphertext: bool,
    submitted_ns: u64,
    published_ns: AtomicU64,
    waiter: *mut c_void,
    state: AtomicU32,
    outcome: UnsafeCell<Option<Result<GetResult, Error>>>,
}

impl Slot {
    /// # Safety
    /// `p` is a slot this worker popped and has not released.
    unsafe fn complete(p: *mut Slot, res: Result<GetResult, Error>) {
        let s = unsafe { &*p };
        let waiter = s.waiter;
        unsafe { *s.outcome.get() = Some(res) };
        s.published_ns.store(unsafe { shim_time_ns() }, Ordering::Relaxed);
        s.state.store(PUBLISHED, Ordering::Release);
        if !waiter.is_null() {
            unsafe { shim_thread_unpark(waiter) };
        }
        s.state.store(RELEASED, Ordering::Release);
    }
}

/// Multi-producer, single-consumer handoff to one worker; one push per request.
struct Queue {
    lock: AtomicBool,
    items: UnsafeCell<VecDeque<*mut Slot>>,
}

unsafe impl Send for Queue {}
unsafe impl Sync for Queue {}

impl Queue {
    fn new() -> Self {
        Self {
            lock: AtomicBool::new(false),
            items: UnsafeCell::new(VecDeque::new()),
        }
    }

    fn acquire(&self) {
        while self
            .lock
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
    }

    fn release(&self) {
        self.lock.store(false, Ordering::Release);
    }

    fn push(&self, slot: *mut Slot) {
        self.acquire();
        unsafe { (*self.items.get()).push_back(slot) };
        self.release();
    }

    fn pop(&self) -> Option<*mut Slot> {
        self.acquire();
        let out = unsafe { (*self.items.get()).pop_front() };
        self.release();
        out
    }
}

pub struct ServiceConfig {
    pub peer: Endpoint,
    pub conns_per_worker: usize,
    pub rx_buffer: usize,
    pub tx_buffer: usize,
}

impl ServiceConfig {
    pub fn new(peer: Endpoint) -> Self {
        Self {
            peer,
            conns_per_worker: 8,
            rx_buffer: 4 * 1024 * 1024,
            tx_buffer: 32 * 1024,
        }
    }
}

/// One thread per RSS queue, each serving requests forever.
pub struct Service {
    queues: Vec<Arc<Queue>>,
    next: AtomicUsize,
}

/// Workers take cpus from the top; cpu 0 is where the application starts.
fn worker_cpu(q: u16) -> usize {
    let cpus = unsafe { crate::ffi::shim_cpu_count() } as usize;
    cpus.saturating_sub(1 + q as usize)
}

impl Service {
    /// Spawn a worker per queue, each pinned to a cpu of its own, never joined.
    pub fn start(stack: &Stack, cfg: &ServiceConfig) -> Result<Service, Error> {
        let mut queues = Vec::with_capacity(stack.queues() as usize);
        for q in 0..stack.queues() {
            let handle = stack.handle(q).ok_or(Error::NoDevice)?;
            let queue = Arc::new(Queue::new());
            let mine = queue.clone();
            let peer = cfg.peer.clone();
            let (conns, rx, tx) = (cfg.conns_per_worker, cfg.rx_buffer, cfg.tx_buffer);
            thread::spawn(
                move || serve(handle, peer, conns, rx, tx, mine),
                Some(worker_cpu(q)),
            );
            queues.push(queue);
        }
        Ok(Service {
            queues,
            next: AtomicUsize::new(0),
        })
    }

    /// Send `head` and write the response body into `buf`. Blocks, parked.
    pub fn get(&self, head: &[u8], buf: &mut [u8]) -> Result<GetResult, Error> {
        self.get_with(head, buf, false)
    }

    /// As [`Service::get`], with the record-layer diagnostic from [`Request`].
    pub fn get_with(
        &self,
        head: &[u8],
        buf: &mut [u8],
        discard_ciphertext: bool,
    ) -> Result<GetResult, Error> {
        if self.queues.is_empty() {
            return Err(Error::NoDevice);
        }
        let t0 = unsafe { shim_time_ns() };
        let slot = Slot {
            head: head.as_ptr(),
            head_len: head.len(),
            buf: buf.as_mut_ptr(),
            buf_cap: buf.len(),
            discard_ciphertext,
            submitted_ns: t0,
            published_ns: AtomicU64::new(0),
            waiter: unsafe { shim_thread_current() },
            state: AtomicU32::new(PENDING),
            outcome: UnsafeCell::new(None),
        };

        let i = self.next.fetch_add(1, Ordering::Relaxed) % self.queues.len();
        self.queues[i].push(&slot as *const Slot as *mut Slot);

        unsafe { shim_thread_park(slot.state.as_ptr() as *const u32) };
        while slot.state.load(Ordering::Acquire) != RELEASED {
            core::hint::spin_loop();
        }
        let t1 = unsafe { shim_time_ns() };
        stats::get_finished(
            t1.saturating_sub(t0),
            t1.saturating_sub(slot.published_ns.load(Ordering::Relaxed)),
        );

        unsafe { (*slot.outcome.get()).take() }.unwrap_or(Err(Error::BadResponse))
    }
}

fn serve(
    handle: WorkerHandle,
    peer: Endpoint,
    conns: usize,
    rx_buffer: usize,
    tx_buffer: usize,
    queue: Arc<Queue>,
) {
    let queue_id = handle.queue_id();
    let mut cfg = WorkerConfig::new(peer);
    cfg.conns = conns;
    cfg.rx_buffer = rx_buffer;
    cfg.tx_buffer = tx_buffer;

    let mut w = match Worker::new(handle, &cfg) {
        Ok(w) => w,
        Err(e) => {
            println!("FAIL: q{}: {}", queue_id, e);
            loop {
                if let Some(p) = queue.pop() {
                    unsafe { Slot::complete(p, Err(e)) };
                }
                core::hint::spin_loop();
            }
        }
    };

    // (slot, was_reused, retried, picked_up_ns)
    let mut pending: Vec<Option<(*mut Slot, bool, bool, u64)>> = Vec::new();
    pending.resize(w.slots(), None);
    let epoch_ns = w.clock().epoch_ns();
    let mut prev_poll_ns = 0u64;
    let mut acc = stats::PollAcc::default();

    loop {
        w.poll();
        let now_ns = w.last_poll_ns();
        if prev_poll_ns != 0 {
            acc.tick(now_ns.saturating_sub(prev_poll_ns), w.last_busy_ns(), w.last_active());
        }
        prev_poll_ns = now_ns;

        for slot in 0..w.slots() {
            match pending[slot] {
                Some((p, was_reused, retried, started_ns)) => {
                    let done = w.conn(slot).and_then(|c| c.outcome());
                    if let Some(step) = done {
                        let res = match step {
                            crate::Step::Complete => {
                                let c = w.conn(slot).expect("just observed");
                                if c.sink_overflowed() {
                                    Err(Error::BufferTooSmall)
                                } else {
                                    Ok(GetResult {
                                        status: c.status(),
                                        written: c.sink_written(),
                                        content_length: c.head().content_length,
                                        content_range: c.head().content_range,
                                        etag: c.head().etag.clone(),
                                        last_modified: c.head().last_modified.clone(),
                                    })
                                }
                            }
                            crate::Step::Failed(e) => Err(e),
                            crate::Step::Pending => unreachable!("outcome() is terminal"),
                        };
                        // A reused socket the peer had already closed: re-dial
                        // once. GET and HEAD are safe to repeat.
                        if was_reused && !retried && matches!(step, crate::Step::Failed(_)) {
                            let s = unsafe { &*p };
                            let head =
                                unsafe { core::slice::from_raw_parts(s.head, s.head_len) };
                            let req = Request {
                                head,
                                discard_ciphertext: s.discard_ciphertext,
                            };
                            w.release(slot);
                            if w.connect_next(slot, &req).is_ok() {
                                stats::request_retried();
                                let sink = unsafe { BufferSink::new(s.buf, s.buf_cap) };
                                if let Some(c) = w.conn_mut(slot) {
                                    c.set_sink(alloc::boxed::Box::new(sink));
                                }
                                pending[slot] = Some((p, false, true, started_ns));
                                continue;
                            }
                        }
                        let head_ns = w.conn(slot).and_then(|c| c.head_ns()).map(|h| h + epoch_ns);
                        stats::request_finished(
                            started_ns.saturating_sub(unsafe { (*p).submitted_ns }),
                            now_ns.saturating_sub(started_ns),
                            res.as_ref().map_or(0, |g| g.written),
                            head_ns.map(|h| h.saturating_sub(started_ns)),
                            head_ns.map(|h| now_ns.saturating_sub(h)),
                        );
                        unsafe { Slot::complete(p, res) };
                        pending[slot] = None;
                        if !w.idle_reusable(slot) {
                            w.release(slot);
                        }
                    }
                }
                None => {
                    let p = match queue.pop() {
                        Some(p) => p,
                        None => continue,
                    };
                    let s = unsafe { &*p };
                    let head = unsafe { core::slice::from_raw_parts(s.head, s.head_len) };
                    let req = Request {
                        head,
                        discard_ciphertext: s.discard_ciphertext,
                    };
                    let reusing = w.idle_reusable(slot);
                    let opened = if reusing {
                        w.reuse(slot, &req)
                    } else {
                        // A kept socket the peer since closed sits in CloseWait;
                        // connect() needs Closed.
                        w.release(slot);
                        w.connect_next(slot, &req)
                    };
                    match opened {
                        Ok(()) => {
                            let sink = unsafe { BufferSink::new(s.buf, s.buf_cap) };
                            if let Some(c) = w.conn_mut(slot) {
                                c.set_sink(alloc::boxed::Box::new(sink));
                            }
                            pending[slot] = Some((p, reusing, false, now_ns));
                        }
                        Err(e) => unsafe { Slot::complete(p, Err(e)) },
                    }
                }
            }
        }
    }
}
