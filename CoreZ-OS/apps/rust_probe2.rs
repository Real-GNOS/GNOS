use std::env;
use std::fs;
use std::io::Write;
use std::os::unix::fs::symlink;
use std::process::Command;
use std::sync::{Arc, Barrier, Mutex, OnceLock, RwLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn run<F>(name: &str, f: F) -> u32
where
    F: FnOnce() -> Result<String, String>,
{
    match f() {
        Ok(d) => {
            print!("rp2 {}: PASS {}\n", name, d);
            1
        }
        Err(e) => {
            print!("rp2 {}: FAIL {}\n", name, e);
            0
        }
    }
}

fn err<E: std::fmt::Display>(e: E) -> String {
    format!("err={}", e)
}

fn step<T, E: std::fmt::Display>(name: &str, r: Result<T, E>) -> Result<T, String> {
    r.map_err(|e| format!("{}:{}", name, e))
}

fn main() {
    let mut pass = 0u32;
    let mut total = 0u32;

    macro_rules! chk {
        ($name:expr, $body:expr) => {{
            total += 1;
            pass += run($name, || $body);
        }};
    }

    chk!("exe", {
        env::current_exe()
            .map(|p| p.display().to_string())
            .map_err(err)
    });

    chk!("procfs", {
        let exe = step("exe", fs::read_link("/proc/self/exe")).map(|p| p.display().to_string());
        let fd = step("fd", fs::read_dir("/proc/self/fd")).map(|d| d.count());
        let maps = step("maps", fs::read_to_string("/proc/self/maps")).map(|s| s.lines().count());
        let st = step("status", fs::read_to_string("/proc/self/status")).map(|s| s.lines().count());
        Ok(format!("exe={:?} fd={:?} maps={:?} status={:?}", exe, fd, maps, st))
    });

    chk!("readdir", {
        let n = step("root", fs::read_dir("/")).map(|d| d.count())?;
        let e = step("etc", fs::read_dir("/etc")).map(|d| d.count())?;
        if n == 0 || e == 0 {
            return Err(format!("empty root={} etc={}", n, e));
        }
        Ok(format!("root={} etc={}", n, e))
    });

    chk!("mkdir", {
        let _ = fs::remove_dir("/tmp/rp2d");
        step("create_dir", fs::create_dir("/tmp/rp2d"))?;
        step("remove_dir", fs::remove_dir("/tmp/rp2d"))?;
        let _ = fs::remove_dir_all("/tmp/rp2m");
        step("create_all", fs::create_dir_all("/tmp/rp2m/x/y"))?;
        let n = step("walk", fs::read_dir("/tmp/rp2m/x")).map(|d| d.count())?;
        step("remove_all", fs::remove_dir_all("/tmp/rp2m"))?;
        Ok(format!("nested={}", n))
    });

    chk!("tmp_state", {
        let mut names: Vec<String> = step("list", fs::read_dir("/tmp"))?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        let md = step("stat", fs::metadata("/tmp")).map(|m| m.is_dir());
        Ok(format!("n={} is_dir={:?} {:?}", names.len(), md, names))
    });

    chk!("fileio", {
        let p = "/tmp/rp2.txt";
        let _ = fs::remove_file(p);
        let mut f = step("create", fs::File::create(p))?;
        step("write", f.write_all(b"hello-rp2\n"))?;
        step("sync", f.sync_all())?;
        drop(f);
        let back = step("read", fs::read_to_string(p))?;
        let md = step("stat", fs::metadata(p))?;
        step("remove", fs::remove_file(p))?;
        if back != "hello-rp2\n" {
            return Err(format!("readback={:?}", back));
        }
        Ok(format!("len={}", md.len()))
    });

    chk!("rename_symlink", {
        let _ = fs::remove_file("/tmp/rp2a.txt");
        let _ = fs::remove_file("/tmp/rp2b.txt");
        let _ = fs::remove_file("/tmp/rp2l");
        step("create", fs::write("/tmp/rp2a.txt", b"x"))?;
        step("rename", fs::rename("/tmp/rp2a.txt", "/tmp/rp2b.txt"))?;
        step("symlink", symlink("rp2b.txt", "/tmp/rp2l"))?;
        let tgt = step("readlink", fs::read_link("/tmp/rp2l"))?;
        let ln = step("lstat", fs::symlink_metadata("/tmp/rp2l")).map(|m| m.file_type().is_symlink());
        step("unlink", fs::remove_file("/tmp/rp2l"))?;
        let _ = fs::remove_file("/tmp/rp2b.txt");
        Ok(format!("tgt={} is_link={:?}", tgt.display(), ln))
    });

    chk!("cmd", {
        let spawned = Command::new("toybox").arg("echo").arg("RP2_CMD_OK").spawn();
        match spawned {
            Ok(mut c) => {
                let st = step("wait", c.wait())?;
                Ok(format!("spawn ok status={}", st.code().unwrap_or(-1)))
            }
            Err(e) => Err(format!("spawn err={} raw={:?}", e, e.raw_os_error())),
        }
    });

    chk!("cmd_out", {
        let out = step("output", Command::new("toybox").arg("echo").arg("RP2_OUT_OK").output())?;
        let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
        Ok(format!("status={} out={:?}", out.status.code().unwrap_or(-1), s))
    });

    chk!("threadcap", {
        let acc = Arc::new(Mutex::new(0u64));
        let mut hs = Vec::new();
        let mut fail: Option<String> = None;
        for _ in 0..64 {
            let a = Arc::clone(&acc);
            match thread::Builder::new().spawn(move || {
                *a.lock().unwrap() += 1;
            }) {
                Ok(h) => hs.push(h),
                Err(e) => {
                    fail = Some(format!("max={} err={} raw={:?}", hs.len(), e, e.raw_os_error()));
                    break;
                }
            }
        }
        let alive = hs.len();
        for h in hs {
            let _ = h.join();
        }
        match fail {
            Some(m) => Err(m),
            None => Ok(format!("max>=64 acc={}", *acc.lock().unwrap())),
        }
    });

    chk!("stack8m", {
        let h = thread::Builder::new()
            .stack_size(8 * 1024 * 1024)
            .spawn(|| 11u32);
        match h {
            Ok(h) => {
                let v = step("join", h.join().map_err(|_| "join".to_string()))?;
                Ok(format!("rv={}", v))
            }
            Err(e) => Err(format!("spawn raw={:?}", e.raw_os_error())),
        }
    });

    chk!("once_rwlock", {
        static ONCE: OnceLock<u32> = OnceLock::new();
        let v = *ONCE.get_or_init(|| 41);
        let rw = Arc::new(RwLock::new(0u32));
        let mut hs = Vec::new();
        for _ in 0..4 {
            let r = Arc::clone(&rw);
            hs.push(thread::spawn(move || {
                for _ in 0..50 {
                    *r.write().unwrap() += 1;
                }
            }));
        }
        for h in hs {
            h.join().map_err(|_| "join failed".to_string())?;
        }
        let got = *rw.read().unwrap();
        if v != 41 || got != 200 {
            return Err(format!("once={} rwlock={}", v, got));
        }
        Ok(format!("once={} rwlock={}", v, got))
    });

    chk!("barrier", {
        let b = Arc::new(Barrier::new(4));
        let hits = Arc::new(Mutex::new(0u32));
        let mut hs = Vec::new();
        for _ in 0..4 {
            let b2 = Arc::clone(&b);
            let h2 = Arc::clone(&hits);
            hs.push(thread::spawn(move || {
                *h2.lock().unwrap() += 1;
                b2.wait();
            }));
        }
        for h in hs {
            h.join().map_err(|_| "join failed".to_string())?;
        }
        Ok(format!("crossed={}", *hits.lock().unwrap()))
    });

    chk!("affinity", {
        let n = thread::available_parallelism().map_err(err)?;
        Ok(format!("cpus={}", n))
    });

    chk!("time", {
        let t0 = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(err)?;
        let i0 = Instant::now();
        thread::sleep(Duration::from_millis(60));
        let d = i0.elapsed();
        if t0.as_secs() < 1_700_000_000 {
            return Err(format!("wall={}", t0.as_secs()));
        }
        if d < Duration::from_millis(30) {
            return Err(format!("mono={:?}", d));
        }
        Ok(format!("wall={} mono={:?}", t0.as_secs(), d))
    });

    chk!("bigvec", {
        let v: Vec<u64> = vec![7u64; 262_144];
        let s: u64 = v.iter().take(1000).sum();
        if let Some(i) = v.iter().position(|&x| x != 7) {
            return Err(format!("bad@{} = {:#x} sum={} len={}", i, v[i], s, v.len()));
        }
        Ok(format!("bytes={}", v.len() * 8))
    });

    chk!("netbind", {
        match std::net::TcpListener::bind("127.0.0.1:0") {
            Ok(l) => {
                let a = l.local_addr().map_err(err)?;
                Ok(format!("bound={}", a))
            }
            Err(e) => Err(format!("bind raw={:?}", e.raw_os_error())),
        }
    });

    print!("rp2 SUMMARY pass={} total={} fail={}\n", pass, total, total - pass);
    if pass != total {
        print!("rust_probe2: FAIL\n");
    } else {
        print!("rust_probe2: PASS\n");
    }
}
