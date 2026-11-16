//! The bench on the Mac: `fm1-bench-host PKG.fwsc [EMU_MS] [--jit]`. Prints a line every 2000
//! emulated ms and a summary in fm1-live's words, to check the P4 build's numbers against.
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let Some(path) = a.get(1) else {
        eprintln!("usage: fm1-bench-host PKG.fwsc [EMU_MS (10000)] [--jit]");
        std::process::exit(2);
    };
    let ms: u64 = a.get(2).and_then(|s| s.parse().ok()).unwrap_or(10_000);
    let jit = a.iter().any(|s| s == "--jit");
    let pkg = std::fs::read(path).expect("read package");
    let mut b = fm1_bench::Bench::new(&pkg, jit).expect("boot package");
    println!("fm1-bench-host: {path} board {} jit {jit}, {} MHz", b.board, fm1_bench::MHZ);
    let t0 = Instant::now();
    let (mut lt, mut lms, mut lcyc, mut lins, mut lc1) = (t0, 0u64, 0u64, 0u64, 0u64);
    let mut steady: Option<(Instant, u64)> = None;
    let mut audio = 0usize;
    while b.ms() < ms {
        if let Err(e) = b.step() {
            eprintln!("halted at {} ms: {e}", b.ms());
            std::process::exit(1);
        }
        if steady.is_none() && b.ms() >= 1000 {
            steady = Some((Instant::now(), b.ms()));
        }
        if b.ms() >= lms + 2000 {
            audio += b.take_audio_len();
            let now = Instant::now();
            let dt = (now - lt).as_secs_f64();
            let c1 = b.cpu1_insns().unwrap_or(0);
            println!(
                "emu {:6} ms  wall {:7.2} s  {:.3}x real time  {:.1} M emu cycles/s  {:.1} M insn/s cpu0  {:.1} M insn/s cpu1",
                b.ms(), (now - t0).as_secs_f64(), (b.ms() - lms) as f64 / 1000.0 / dt,
                (b.cycles() - lcyc) as f64 / dt / 1e6, (b.insns - lins) as f64 / dt / 1e6, (c1 - lc1) as f64 / dt / 1e6
            );
            (lt, lms, lcyc, lins, lc1) = (now, b.ms(), b.cycles(), b.insns, c1);
        }
    }
    audio += b.take_audio_len();
    let wall = t0.elapsed().as_secs_f64();
    let st = steady.map(|(t, s)| (b.ms() - s) as f64 / 1000.0 / t.elapsed().as_secs_f64());
    println!("steady: {st:?}x real time; {} audio frames; cpu1 {:?} instructions", audio / 2, b.cpu1_insns());
    println!(
        "{} ms emulated at {} MHz in {:.2} s wall: {:.2}x real time, {:.1} M instructions/s, {:.0}% of cycles skipped as idle",
        b.ms(), fm1_bench::MHZ, wall, b.ms() as f64 / 1000.0 / wall, b.insns as f64 / wall / 1e6,
        100.0 * b.skipped() as f64 / b.cycles().max(1) as f64
    );
}
