# Outrigger Synth

A separate track: Outrigger as a standalone synth with no FM-1. It runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5,
with a Launchkey and a USB DAC on a powered hub, and Felucca's sound engines compiled from source.

| Doc | What it holds |
|---|---|
| [p4-synth-proposal.md](p4-synth-proposal.md) | **Start here.** The plan, the board's verified USB facts, what Outrigger code is reused, the steps, the risks, and the probe results |
| [synth-engine-scope.md](synth-engine-scope.md) | The research behind it: Schwung, engines and licenses (AMY, ESP32Synth, Felucca…), CPU and USB contention, FM-1 firmware without an FM-1 |

**Status (2026-11-03):** the step 0 USB probe passed, and step 1's 10-minute tone soak had 0 underruns.
- **Probe code and results:** branch `claude/synth-p4-probe-05de29` (`prototypes/p4_usb_probe/`, `docs/p4-usb-probe.md`).
- **Next:** step 2, Felucca rendering into the DAC from the Launchkey, with render times measured.

Both docs started as research notes outside the repo. Paths they mention under "side-session-notes" are from that
time; these copies are the ones to keep.
