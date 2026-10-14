//! cr-rust-hello: the Rust half of milestone CR-a (docs/CR-PLAN.md, track
//! CT2). One group of lines for each thing a Rust program leans on: threads,
//! channels, files, std's collections and number formatting, then (with the
//! argument `panic`) a panic that aborts. A check that fails prints
//! "cr-rust-hello: FAIL <what>" and the exit status is 1. The last line is
//! "cr-rust-hello: ok".
//!
//!   cr-rust-hello [directory]   the files section's scratch directory
//!                               (default /data/cr-rust-hello; made, removed)
//!   cr-rust-hello panic         a thread panics: the hook prints, and the
//!                               process aborts (SIGABRT)

use std::cell::Cell;
use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, ErrorKind, Read, Seek, SeekFrom, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Condvar, Mutex, OnceLock, RwLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

static FAILURES: AtomicUsize = AtomicUsize::new(0);

fn check(ok: bool, what: &str) {
    if !ok {
        println!("cr-rust-hello: FAIL {}", what);
        FAILURES.fetch_add(1, Ordering::SeqCst);
    }
}

/* ---- threads ------------------------------------------------------------ */

static TL_DROPS: AtomicUsize = AtomicUsize::new(0);

struct Noted(u64);

impl Drop for Noted {
    fn drop(&mut self) {
        TL_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

thread_local! {
    static NOTE: Noted = Noted(7);
    static COUNT: Cell<u32> = const { Cell::new(0) };
}

fn section_threads() {
    // Eight threads add up their own million numbers.
    let handles: Vec<_> = (0..8u64)
        .map(|i| {
            thread::spawn(move || {
                NOTE.with(|n| assert_eq!(n.0, 7));
                (i * 1_000_000..(i + 1) * 1_000_000).sum::<u64>()
            })
        })
        .collect();
    let total: u64 = handles.into_iter().map(|h| h.join().unwrap()).sum();
    println!("threads: 8 joined, total {}", total);
    check(total == 31_999_996_000_000, "the threads' total");

    // Thread-local destructors run when a thread ends, before it is joined.
    COUNT.with(|c| c.set(c.get() + 1));
    NOTE.with(|n| assert_eq!(n.0, 7));
    println!(
        "threads: thread_local destructors ran {}, main's count {}",
        TL_DROPS.load(Ordering::SeqCst),
        COUNT.with(|c| c.get())
    );
    check(TL_DROPS.load(Ordering::SeqCst) == 8, "the thread_local destructors");

    // A mutex and an atomic under eight threads.
    let counter = Arc::new(Mutex::new(0u32));
    let hits = Arc::new(AtomicUsize::new(0));
    let handles: Vec<_> = (0..8)
        .map(|_| {
            let counter = Arc::clone(&counter);
            let hits = Arc::clone(&hits);
            thread::spawn(move || {
                for _ in 0..1000 {
                    *counter.lock().unwrap() += 1;
                    hits.fetch_add(1, Ordering::Relaxed);
                }
            })
        })
        .collect();
    for h in handles {
        h.join().unwrap();
    }
    println!(
        "threads: mutex {}, atomic {}",
        *counter.lock().unwrap(),
        hits.load(Ordering::SeqCst)
    );
    check(*counter.lock().unwrap() == 8000 && hits.load(Ordering::SeqCst) == 8000, "the counters");

    // A condition variable: the waiter wakes when the flag is set.
    let pair = Arc::new((Mutex::new(false), Condvar::new()));
    let waiter = {
        let pair = Arc::clone(&pair);
        thread::spawn(move || {
            let (lock, cv) = &*pair;
            let mut ready = lock.lock().unwrap();
            while !*ready {
                ready = cv.wait(ready).unwrap();
            }
            "woken"
        })
    };
    thread::sleep(Duration::from_millis(5));
    {
        let (lock, cv) = &*pair;
        *lock.lock().unwrap() = true;
        cv.notify_all();
    }
    println!("threads: condvar {}", waiter.join().unwrap());

    // Scoped threads borrow the caller's data; a RwLock and a OnceLock.
    let data = vec![1u32, 2, 3, 4, 5, 6, 7, 8];
    let sum = RwLock::new(0u32);
    let once = OnceLock::new();
    let inits = AtomicUsize::new(0);
    thread::scope(|s| {
        for chunk in data.chunks(2) {
            let sum = &sum;
            let once = &once;
            let inits = &inits;
            s.spawn(move || {
                *sum.write().unwrap() += chunk.iter().sum::<u32>();
                once.get_or_init(|| {
                    inits.fetch_add(1, Ordering::SeqCst);
                    99
                });
            });
        }
    });
    println!(
        "threads: scoped sum {}, OnceLock {} made {} time(s)",
        *sum.read().unwrap(),
        once.get().unwrap(),
        inits.load(Ordering::SeqCst)
    );

    // A named thread with a small stack, and park/unpark.
    let b = thread::Builder::new().name("small".to_string()).stack_size(128 * 1024);
    let h = b
        .spawn(|| {
            thread::park();
            thread::current().name().map(|s| s.to_string())
        })
        .unwrap();
    thread::sleep(Duration::from_millis(5));
    h.thread().unpark();
    println!("threads: named {:?}", h.join().unwrap());

    // (A panic in a thread is the abort mode's: the panic strategy is abort.)
    let v = thread::spawn(|| vec!["a", "b", "c"].join("-")).join().unwrap();
    println!("threads: returned {}", v);

    let t = Instant::now();
    thread::sleep(Duration::from_millis(10));
    check(t.elapsed() >= Duration::from_millis(10), "sleep");
    println!("threads: slept at least 10 ms");
    check(thread::available_parallelism().map(|n| n.get() >= 1).unwrap_or(true), "parallelism");
}

/* ---- channels ----------------------------------------------------------- */

fn section_channels() {
    // Four producers, one consumer: the total, and each producer's order.
    let (tx, rx) = mpsc::channel::<(usize, usize)>();
    let producers: Vec<_> = (0..4)
        .map(|id| {
            let tx = tx.clone();
            thread::spawn(move || {
                for i in 0..100 {
                    tx.send((id, i)).unwrap();
                }
            })
        })
        .collect();
    drop(tx);
    let mut sum = 0;
    let mut count = 0;
    let mut last = [None::<usize>; 4];
    let mut in_order = true;
    for (id, i) in rx {
        sum += id * 1000 + i;
        count += 1;
        if let Some(l) = last[id] {
            in_order &= i == l + 1;
        }
        last[id] = Some(i);
    }
    for p in producers {
        p.join().unwrap();
    }
    println!("channels: {} messages, sum {}, each producer in order {}", count, sum, in_order);
    check(count == 400 && sum == 619_800 && in_order, "the channel");

    // A rendezvous channel and a ping-pong over two of them.
    let (ping_tx, ping_rx) = mpsc::sync_channel::<u32>(0);
    let (pong_tx, pong_rx) = mpsc::sync_channel::<u32>(0);
    let echo = thread::spawn(move || {
        for n in ping_rx {
            pong_tx.send(n * 2).unwrap();
        }
    });
    let mut seen = Vec::new();
    for n in 1..=5 {
        ping_tx.send(n).unwrap();
        seen.push(pong_rx.recv().unwrap());
    }
    drop(ping_tx);
    echo.join().unwrap();
    println!("channels: ping-pong {:?}", seen);

    // Timeouts, a try, and a closed channel.
    let (tx, rx) = mpsc::channel::<u8>();
    check(rx.try_recv() == Err(mpsc::TryRecvError::Empty), "try_recv on an empty channel");
    check(
        rx.recv_timeout(Duration::from_millis(20)) == Err(mpsc::RecvTimeoutError::Timeout),
        "recv_timeout",
    );
    tx.send(9).unwrap();
    drop(tx);
    check(rx.recv() == Ok(9), "a queued message survives the sender");
    check(rx.recv().is_err(), "recv on a closed channel");
    println!("channels: empty, timeout, queued 9, then closed");

    // A pool of three workers taking boxed jobs from one shared receiver.
    type Job = Box<dyn FnOnce() -> u64 + Send>;
    let (job_tx, job_rx) = mpsc::channel::<Job>();
    let job_rx = Arc::new(Mutex::new(job_rx));
    let (res_tx, res_rx) = mpsc::channel::<u64>();
    let workers: Vec<_> = (0..3)
        .map(|_| {
            let job_rx = Arc::clone(&job_rx);
            let res_tx = res_tx.clone();
            thread::spawn(move || loop {
                let job = job_rx.lock().unwrap().recv();
                match job {
                    Ok(job) => res_tx.send(job()).unwrap(),
                    Err(_) => break,
                }
            })
        })
        .collect();
    drop(res_tx);
    for n in 1..=10u64 {
        job_tx.send(Box::new(move || n * n)).unwrap();
    }
    drop(job_tx);
    for w in workers {
        w.join().unwrap();
    }
    let mut results: Vec<u64> = res_rx.iter().collect();
    results.sort();
    println!("channels: pool squares {:?}", results);
}

/* ---- files -------------------------------------------------------------- */

fn section_files(dir: &str) {
    let root = PathBuf::from(dir);
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(root.join("a/b")).unwrap();

    fs::write(root.join("a/uno.txt"), "one\n").unwrap();
    let mut f = File::create(root.join("a/b/two.txt")).unwrap();
    for i in 0..500 {
        writeln!(f, "line {:03}", i).unwrap();
    }
    f.sync_all().unwrap();
    drop(f);
    println!(
        "files: uno.txt says {:?}, two.txt is {} bytes",
        fs::read_to_string(root.join("a/uno.txt")).unwrap().trim_end(),
        fs::metadata(root.join("a/b/two.txt")).unwrap().len()
    );

    // Append, then read by lines, then seek into the middle.
    let mut f = OpenOptions::new().append(true).open(root.join("a/uno.txt")).unwrap();
    f.write_all(b"two\nthree\n").unwrap();
    drop(f);
    let lines: Vec<String> = BufReader::new(File::open(root.join("a/uno.txt")).unwrap())
        .lines()
        .map(|l| l.unwrap())
        .collect();
    println!("files: appended, {} lines {:?}", lines.len(), lines);

    let mut f = File::open(root.join("a/b/two.txt")).unwrap();
    f.seek(SeekFrom::Start(9 * 250)).unwrap();
    let mut buf = [0u8; 8];
    f.read_exact(&mut buf).unwrap();
    let end = f.seek(SeekFrom::End(0)).unwrap();
    println!("files: at 2250 {:?}, end {}", String::from_utf8_lossy(&buf), end);
    drop(f);

    // A directory listing (sorted: the order is the file system's), a copy,
    // a rename and a read-write open.
    let mut names: Vec<String> = fs::read_dir(root.join("a"))
        .unwrap()
        .map(|e| {
            let e = e.unwrap();
            let kind = if e.file_type().unwrap().is_dir() { "/" } else { "" };
            format!("{}{}", e.file_name().to_string_lossy(), kind)
        })
        .collect();
    names.sort();
    println!("files: a/ holds {:?}", names);

    let copied = fs::copy(root.join("a/b/two.txt"), root.join("copy.txt")).unwrap();
    fs::rename(root.join("copy.txt"), root.join("moved.txt")).unwrap();
    let same =
        fs::read(root.join("moved.txt")).unwrap() == fs::read(root.join("a/b/two.txt")).unwrap();
    println!("files: copied {} bytes, renamed, same {}", copied, same);
    check(same && copied == 4500, "the copy");

    let mut f = OpenOptions::new().read(true).write(true).open(root.join("moved.txt")).unwrap();
    f.seek(SeekFrom::Start(5)).unwrap();
    f.write_all(b"XYZ").unwrap();
    f.seek(SeekFrom::Start(0)).unwrap();
    let mut first = String::new();
    BufReader::new(&f).read_line(&mut first).unwrap();
    println!("files: rewritten first line {:?}", first.trim_end());
    drop(f);

    // Errors: their kinds and the text of the system's message.
    let e = File::open(root.join("not-there")).unwrap_err();
    println!("files: open of a missing file: {:?}, {}", e.kind(), e);
    check(e.kind() == ErrorKind::NotFound, "NotFound");
    let e = fs::create_dir(root.join("a")).unwrap_err();
    println!("files: create_dir over a directory: {:?}", e.kind());
    check(e.kind() == ErrorKind::AlreadyExists, "AlreadyExists");
    check(root.join("a/uno.txt").is_file() && root.join("a/b").is_dir(), "is_file, is_dir");

    // Threads writing files of their own side by side.
    let handles: Vec<_> = (0..4)
        .map(|i| {
            let p = root.join(format!("w{}.txt", i));
            thread::spawn(move || {
                fs::write(&p, format!("writer {}\n", i).repeat(i + 1)).unwrap();
                fs::metadata(&p).unwrap().len()
            })
        })
        .collect();
    let sizes: Vec<u64> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    println!("files: four writers made {:?} bytes", sizes);

    fs::remove_file(root.join("a/uno.txt")).unwrap();
    let removed = fs::remove_dir_all(&root).is_ok();
    println!("files: remove_all done {}, the directory is there {}", removed, root.exists());
    check(removed && !root.exists(), "remove_dir_all");
}

/* ---- the library -------------------------------------------------------- */

fn section_library() {
    // A hash map (std seeds it from the system), a tree map, sorting.
    let mut words: HashMap<String, u32> = HashMap::new();
    for i in 0..2000u32 {
        *words.entry(format!("k{}", i % 97)).or_insert(0) += i;
    }
    let mut keys: Vec<_> = words.iter().map(|(k, v)| (k.clone(), *v)).collect();
    keys.sort();
    let tree: BTreeMap<_, _> = keys.iter().cloned().collect();
    println!(
        "library: {} keys, k0={}, k96={}, first {:?}",
        words.len(),
        words["k0"],
        tree["k96"],
        tree.iter().next().unwrap()
    );
    let mut v = vec![5, 3, 9, 1, 7];
    v.sort_by(|a, b| b.cmp(a));
    println!("library: sorted {:?}", v);

    // Text: formatting, parsing, Unicode.
    println!(
        "library: [{:>6}|{:<6}|{:^6}|{:06.2}|{:#x}|{:e}]",
        42, "ab", "mid", 3.14159, 255, 12345.678
    );
    let n: i64 = "-89".parse().unwrap();
    let f: f64 = "0.25".parse().unwrap();
    println!("library: parsed {} {} {:?}", n, f, "x".parse::<u8>().is_err());
    println!(
        "library: {} {}",
        "straße ñandú".to_uppercase(),
        "héllo".chars().rev().collect::<String>()
    );

    // Wide integers and floats (compiler_builtins' routines).
    let big: u128 = 0xffff_ffff_ffff_ffff_u128 * 0x1_0000_0001_u128 + 12345;
    let q = big / 1_000_000_007;
    let r = big % 1_000_000_007;
    println!("library: u128 {} / 1000000007 = {} rem {}", big, q, r);
    let x: i128 = i128::MIN;
    println!("library: i128 min {}, sqrt2 {:.10}, powi {}", x, 2f64.sqrt(), 1.5f64.powi(10));

    // Boxes, trait objects, reference counts.
    trait Shape {
        fn area(&self) -> f64;
    }
    struct Square(f64);
    struct Circle(f64);
    impl Shape for Square {
        fn area(&self) -> f64 {
            self.0 * self.0
        }
    }
    impl Shape for Circle {
        fn area(&self) -> f64 {
            3.0 * self.0 * self.0
        }
    }
    let shapes: Vec<Box<dyn Shape>> = vec![Box::new(Square(2.0)), Box::new(Circle(1.0))];
    let rc = std::rc::Rc::new(5);
    let rc2 = std::rc::Rc::clone(&rc);
    println!(
        "library: areas {:?}, Rc count {}",
        shapes.iter().map(|s| s.area()).collect::<Vec<_>>(),
        std::rc::Rc::strong_count(&rc2)
    );

    // The clock, the process, the arguments.
    check(SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() > 1_000_000_000, "the clock");
    check(std::process::id() > 0, "process id");
    println!("library: arguments {}", std::env::args().count());
}

/* ---- the abort ----------------------------------------------------------- */

fn section_panic() -> ! {
    println!("cr-rust-hello: panic mode");
    std::panic::set_hook(Box::new(|info| {
        let msg = if let Some(s) = info.payload().downcast_ref::<&str>() {
            s.to_string()
        } else if let Some(s) = info.payload().downcast_ref::<String>() {
            s.clone()
        } else {
            "?".to_string()
        };
        let name = thread::current().name().unwrap_or("?").to_string();
        println!("cr-rust-hello: panic in thread '{}': {}", name, msg);
    }));
    let h = thread::Builder::new()
        .name("worker".to_string())
        .spawn(|| {
            let n = 42;
            panic!("boom {}", n);
        })
        .unwrap();
    // Not reached with panic=abort: the process is gone before this returns.
    let _ = h.join();
    println!("cr-rust-hello: FAIL the panic did not abort");
    std::process::exit(1);
}

fn main() {
    let arg = std::env::args().nth(1);
    if arg.as_deref() == Some("panic") {
        section_panic();
    }
    let dir = arg.unwrap_or_else(|| "/data/cr-rust-hello".to_string());
    println!("cr-rust-hello: start");
    section_threads();
    section_channels();
    section_files(&dir);
    section_library();
    if FAILURES.load(Ordering::SeqCst) != 0 {
        std::process::exit(1);
    }
    println!("cr-rust-hello: ok");
}
