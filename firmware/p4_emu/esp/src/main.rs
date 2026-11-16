//! fm1-emu's core on the ESP32-P4: boots the FM-1 package in the `fwpkg` partition (written there
//! by tools/mkpkg.py's image) headless, and prints its emulated speed every 5 s on the console.
//! One emulator thread, pinned to the P4's core 1, runs both emulated FM-1 cores (as fm1-live does).

use esp_idf_sys as sys;
use std::time::{Duration, Instant};

const MAGIC: &[u8; 8] = b"FM1PKG\0\x01";
const HEADER: usize = 64;
const REPORT: Duration = Duration::from_secs(5);

fn read_package() -> Result<(String, Vec<u8>), String> {
    // SAFETY: plain IDF calls; the label is NUL-terminated and the buffers are ours
    unsafe {
        let p = sys::esp_partition_find_first(
            sys::esp_partition_type_t_ESP_PARTITION_TYPE_DATA,
            sys::esp_partition_subtype_t_ESP_PARTITION_SUBTYPE_ANY,
            c"fwpkg".as_ptr(),
        );
        if p.is_null() {
            return Err("no fwpkg partition (flash partitions.csv's table)".into());
        }
        let size = (*p).size as usize;
        let mut h = [0u8; HEADER];
        let e = sys::esp_partition_read(p, 0, h.as_mut_ptr().cast(), HEADER);
        if e != 0 {
            return Err(format!("esp_partition_read header: {e}"));
        }
        if &h[..8] != MAGIC {
            return Err("fwpkg holds no package: write tools/mkpkg.py's image at 0x410000".into());
        }
        let len = u32::from_le_bytes(h[8..12].try_into().unwrap()) as usize;
        if len == 0 || HEADER + len > size {
            return Err(format!("package length {len} does not fit the {size} byte partition"));
        }
        let name_end = h[12..].iter().position(|&b| b == 0).map_or(HEADER, |i| 12 + i);
        let name = String::from_utf8_lossy(&h[12..name_end]).into_owned();
        let mut pkg = vec![0u8; len];
        let e = sys::esp_partition_read(p, HEADER, pkg.as_mut_ptr().cast(), len);
        if e != 0 {
            return Err(format!("esp_partition_read package: {e}"));
        }
        Ok((name, pkg))
    }
}

fn free(caps: u32) -> usize {
    // SAFETY: a read-only heap query
    unsafe { sys::heap_caps_get_free_size(caps) }
}

fn run(name: String, pkg: Vec<u8>) {
    let t_boot = Instant::now();
    let mut b = match fm1_bench::Bench::new(&pkg, false) {
        Ok(b) => b,
        Err(e) => {
            println!("FM1EMU error: cannot boot {name}: {e}");
            return;
        }
    };
    drop(pkg);
    println!(
        "FM1EMU boot {name} board {} in {} ms; free internal {} PSRAM {}",
        b.board,
        t_boot.elapsed().as_millis(),
        free(sys::MALLOC_CAP_INTERNAL),
        free(sys::MALLOC_CAP_SPIRAM)
    );
    let t0 = Instant::now();
    let (mut lt, mut lms, mut lcyc, mut lins, mut lc1) = (t0, 0u64, 0u64, 0u64, 0u64);
    loop {
        if let Err(e) = b.step() {
            println!("FM1EMU halted at {} ms: {e}", b.ms());
            return;
        }
        let now = Instant::now();
        if now - lt >= REPORT {
            let audio = b.take_audio_len();
            let dt = (now - lt).as_secs_f64();
            let c1 = b.cpu1_insns().unwrap_or(0);
            let skipped = 100.0 * b.skipped() as f64 / b.cycles().max(1) as f64;
            let (dec, built, resets) = b.diag();
            // one line per report, key=value, for a script to parse
            println!(
                "FM1EMU fw={name} emu_ms={} wall_s={:.1} ratio={:.4} ratio_total={:.4} emu_mcyc_s={:.2} cpu0_minsn_s={:.2} cpu1_minsn_s={:.2} cpu1={} audio_frames={} skipped_pct={:.0} free_int={} free_psram={} decodes={dec} built={built} resets={resets} min_free_psram={}",
                b.ms(),
                (now - t0).as_secs_f64(),
                (b.ms() - lms) as f64 / 1000.0 / dt,
                b.ms() as f64 / 1000.0 / (now - t0).as_secs_f64(),
                (b.cycles() - lcyc) as f64 / dt / 1e6,
                (b.insns - lins) as f64 / dt / 1e6,
                (c1 - lc1) as f64 / dt / 1e6,
                b.cpu1_insns().is_some(),
                audio / 2,
                skipped,
                free(sys::MALLOC_CAP_INTERNAL),
                free(sys::MALLOC_CAP_SPIRAM),
                // SAFETY: a read-only heap query
                unsafe { sys::heap_caps_get_minimum_free_size(sys::MALLOC_CAP_SPIRAM) },
            );
            (lt, lms, lcyc, lins, lc1) = (now, b.ms(), b.cycles(), b.insns, c1);
        }
    }
}

fn main() {
    sys::link_patches();
    extern "C" {
        // esp_private/esp_clk.h: the CPU clock in Hz (not in esp-idf-sys's bindings)
        fn esp_clk_cpu_freq() -> i32;
    }
    // SAFETY: a read of the clock the startup code set
    let mhz = unsafe { esp_clk_cpu_freq() } / 1_000_000;
    println!("FM1EMU start: ESP32-P4 at {mhz} MHz, emulated FM-1 at {} MHz, fm1-emu 60645a58", fm1_bench::MHZ);
    let (name, pkg) = match read_package() {
        Ok(v) => v,
        Err(e) => {
            println!("FM1EMU error: {e}");
            loop {
                std::thread::sleep(Duration::from_secs(10));
            }
        }
    };
    println!("FM1EMU package {name}: {} bytes", pkg.len());
    // the emulator thread on core 1, with a deep stack (the interpreter recurses little, but the
    // SoC's structs are big); core 0 keeps the console and the IDF's own tasks
    // SAFETY: the IDF's pthread defaults for the next thread this task creates
    unsafe {
        let mut cfg = sys::esp_pthread_get_default_config();
        cfg.stack_size = 96 * 1024;
        cfg.pin_to_core = 1;
        cfg.prio = 5;
        sys::esp_pthread_set_cfg(&cfg);
    }
    let h = std::thread::Builder::new()
        .stack_size(96 * 1024)
        .spawn(move || run(name, pkg))
        .expect("spawn the emulator thread");
    let _ = h.join();
    println!("FM1EMU stopped");
    loop {
        std::thread::sleep(Duration::from_secs(10));
    }
}
