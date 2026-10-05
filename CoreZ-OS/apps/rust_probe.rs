use std::collections::HashMap;
use std::sync::{Arc, Condvar, Mutex};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

fn main() {
    print!("rust: S1 boot ok\n");
    println!("rust: hello from corez-os");
    print!("rust: S2 argv={} cwd={:?}\n", std::env::args().count(),
           std::env::current_dir().map(|p| p.display().to_string()));

    let v: Vec<u32> = (1..=10).collect();
    let s = format!("sum={} len={}", v.iter().sum::<u32>(), v.len());
    print!("rust: S3 vec/string {}\n", s);

    let mut m: HashMap<&str, u32> = HashMap::new();
    m.insert("a", 1);
    m.insert("b", 2);
    print!("rust: S4 hashmap a={} b={}\n", m["a"], m["b"]);

    let passwd = std::fs::read_to_string("/etc/passwd").unwrap_or_default();
    print!("rust: S5 fs len={} lines={}\n", passwd.len(),
           passwd.lines().count());

    let t0 = Instant::now();
    thread::sleep(Duration::from_millis(120));
    let e = t0.elapsed();
    print!("rust: S6 sleep {:?}\n", e);

    let h = thread::spawn(|| 7u64);
    thread::sleep(Duration::from_millis(300));
    print!("rust: S7 join {}\n", h.join().unwrap());

    let mtx = Arc::new(Mutex::new(0u64));
    let mut hs = Vec::new();
    for i in 0..4 {
        let m = Arc::clone(&mtx);
        hs.push(thread::spawn(move || {
            print!("rust: S8 t{} up\n", i);
            for _ in 0..200 {
                *m.lock().unwrap() += 1;
            }
            print!("rust: S8 t{} done\n", i);
        }));
    }
    print!("rust: S8 spawned\n");
    for (i, x) in hs.into_iter().enumerate() {
        x.join().unwrap();
        print!("rust: S8 joined {}\n", i);
    }
    print!("rust: S8 mutex {}\n", *mtx.lock().unwrap());

    let pair = Arc::new((Mutex::new(false), Condvar::new()));
    let p2 = Arc::clone(&pair);
    let t = thread::spawn(move || {
        thread::sleep(Duration::from_millis(150));
        let (l, c) = &*p2;
        *l.lock().unwrap() = true;
        c.notify_one();
    });
    let (l, c) = &*pair;
    let mut g = l.lock().unwrap();
    let mut waited = 0;
    while !*g {
        let (ng, r) = c.wait_timeout(g, Duration::from_millis(2000)).unwrap();
        g = ng;
        waited += 1;
        if r.timed_out() {
            break;
        }
    }
    drop(g);
    t.join().unwrap();
    print!("rust: S9 condvar waited={} set={}\n", waited, *l.lock().unwrap());

    let (l2, c2) = &*pair;
    let g = l2.lock().unwrap();
    let (g2, r) = c2.wait_timeout(g, Duration::from_millis(150)).unwrap();
    drop(g2);
    print!("rust: S10 condvar timeout={}\n", r.timed_out());

    let (tx, rx) = mpsc::channel::<u32>();
    thread::spawn(move || {
        thread::sleep(Duration::from_millis(100));
        tx.send(42).unwrap();
    });
    print!("rust: S11 mpsc recv={} timeout={}\n", rx.recv().unwrap(),
           rx.recv_timeout(Duration::from_millis(120)).is_err());

    print!("rust_probe: PASS\n");
}
