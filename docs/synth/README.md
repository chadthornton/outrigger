# Outrigger Synth

A separate track: Outrigger as a standalone synth with no FM-1. It runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5,
with a Launchkey and a USB DAC on a powered hub, and Felucca's sound engines compiled from source.

| Doc | What it holds |
|---|---|
| [SHELVED.md](SHELVED.md) | **Start here.** The track is shelved: what was proven, where the code is, open questions, how to resume |
| [p4-synth-proposal.md](p4-synth-proposal.md) | **Read second.** The plan, the board's verified USB facts, what Outrigger code is reused, the steps, the risks, and the probe results |
| [p4-usb-probe.md](p4-usb-probe.md) | Steps 0 and 1 on Big-P4: hub, Launchkey and USB DAC through the OTG port, the tone soak, log excerpts |
| [s3-first-sound.md](s3-first-sound.md) | Felucca on Big-S3 (PR #54): render µs per engine, the I2S tone soak, Felucca host vs fm1-base |
| [synth-engine-scope.md](synth-engine-scope.md) | The research behind it: Schwung, engines and licenses (AMY, ESP32Synth, Felucca…), CPU and USB contention, FM-1 firmware without an FM-1 |

**Status (2026-11-05): shelved after step 1.** Steps 0 and 1 passed on Big-P4, and Felucca fits one S3 core
(PR #54, `lib/synth`). The probe sketch is on branch `claude/synth-p4-probe-05de29`. Next, when resumed: step 2,
Felucca rendering into the DAC from the Launchkey.

The proposal, the engine scope and the S3 notes started as research notes outside the repo. Paths they mention under "side-session-notes" are from that
time; these copies are the ones to keep.
