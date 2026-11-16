//! A headless FM-1 run for speed, the same machine and bench input as fm1-emu's
//! `fm1-live --bench` (crates/fm1-live/src/main.rs, `Machine::new` and `bench`), without the
//! window, audio device or files, so it builds for the ESP32-P4 too.
//!
//! fm1-core compiles firmware code to native code only on AArch64 and x86-64 hosts; on any other
//! target (the P4's riscv32) it runs its block interpreter, the path `FM1_NO_JIT=1` selects on a
//! Mac. `Bench::new(pkg, jit)` lets the Mac host run either, to compare with the P4.

use fm1_core::{Bus, Cpu};
use fm1_soc::{machine, Action, Soc};

/// The emulated RAM, as fm1-live (1 MiB).
pub const RAM_SIZE: usize = 1 << 20;
/// The real FM-1's clock, per core (JieLi AC791N), and fm1-live's default `--mhz`.
pub const MHZ: u64 = 240;
/// Instructions per `run` call, as fm1-live's bench loop.
pub const CHUNK: u32 = 100_000;

pub struct Bench {
    cpu: Cpu,
    cpu1: Option<Cpu>,
    soc: Soc,
    pending_tick: bool,
    /// cpu0 instructions run (counted as fm1-live counts them: CHUNK per call)
    pub insns: u64,
    /// the package's board and name, for the report
    pub board: String,
}

impl Bench {
    /// A machine booted from a firmware package (.fwsc), with fm1-live's default bench input:
    /// panel keys 21, 25 and 28 pressed at 800, 900 and 1000 ms and held.
    pub fn new(pkg: &[u8], jit: bool) -> Result<Bench, String> {
        let st = fm1_soc::stock_from_package(pkg, None)?;
        let mut soc = Soc::new(RAM_SIZE, st.xip.clone());
        soc.set_flash_part(Default::default());
        soc.periph.cycle_ticks = 1;
        soc.periph.hz = MHZ * 1_000_000;
        soc.lcd_timed = false;
        soc.periph.skip_idle = true;
        let mut cpu = Cpu::new(st.entry);
        cpu.jit_on = jit;
        cpu.user = true;
        let params = fm1_soc::RAM_BASE + 0x7fc00;
        soc.write32(params, params + 0x40);
        cpu.regs[0] = fm1_soc::stock_boot(&mut soc, &st.flash, params, st.board);
        soc.sfc_boot(st.key, st.app_end);
        for (ms, id) in [(800, 21), (900, 25), (1000, 28)] {
            let at = soc.periph.cycle_of_ms(ms);
            soc.periph.script.push_back((at, Action::Key { id, down: true }));
        }
        let board = format!("{:?}", st.board);
        Ok(Bench { cpu, cpu1: None, soc, pending_tick: false, insns: 0, board })
    }

    /// Run one chunk of cpu0 instructions, and cpu1 alongside once the firmware releases it.
    pub fn step(&mut self) -> Result<(), String> {
        let jit = self.cpu.jit_on;
        let Bench { cpu, cpu1, soc, pending_tick, .. } = self;
        machine::run_dual(cpu1, soc, CHUNK, RAM_SIZE, jit, |soc, k| machine::run_blocks(cpu, soc, k, pending_tick))?;
        self.insns += CHUNK as u64;
        Ok(())
    }

    /// Emulated milliseconds since the boot.
    pub fn ms(&self) -> u64 {
        self.soc.periph.ms()
    }

    /// Emulated cycles since the boot (one core's clock).
    pub fn cycles(&self) -> u64 {
        self.soc.periph.cycles
    }

    /// Of those, the cycles skipped as idle (polling loops, waits).
    pub fn skipped(&self) -> u64 {
        self.soc.periph.skipped
    }

    /// Core 1's instructions, if the firmware started it.
    pub fn cpu1_insns(&self) -> Option<u64> {
        self.cpu1.as_ref().map(|c| c.insn_count)
    }

    /// Audio samples produced since the last call (stereo i16 at ~44.1 kHz), dropped so a long
    /// run's memory stays flat: fm1-soc keeps every sample for fm1-live's WAV output.
    pub fn take_audio_len(&mut self) -> usize {
        let n = self.soc.periph.audio.len();
        self.soc.periph.audio.clear();
        n
    }
}
