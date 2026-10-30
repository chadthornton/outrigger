# Mistakes, and how each was caught

[Index](index.md) · previous: [How robustness was earned](robustness.md) · next: [Open questions](open-questions.md)

The AI sessions made these mistakes, or missed these bugs, and something else caught them. Each entry says what was
wrong, what caught it, and what changed. They're in the order they happened.

## 1. A channel count that ignored Skiff's serial port

- **Missed:** nobody had counted the FM-1's serial port. At about 3 channels per MIDI device, hub + FM-1 +
  Launchkey looked like 8.
- **Caught by:** the board. The Launchkey enumerated with its name but delivered no MIDI. Skiff's CDC serial port
  cost 2 more channels, so the rig needed 10.
- **Changed:** CDC claiming is off in the vendored library (`cb4ee7b`), and channel cost is counted from
  descriptors, not per-device guesses.

## 2. The wrong reset command

- **Wrong:** the first plan (`1c62a48`) reset the FM-1 with `0xFC02`.
- **Caught by:** porting from jl-uboot-tool for the golden vectors. `0xFC02` is the V1 loader's reset, and the wl82
  runs V2, whose reset is `0xFC0C` with argument 1.
- **Confirmed by:** checkpoint 1, where the FM-1 restarted into its firmware (`8d77a6c`, `cbfbae3`).

## 3. Sending before the device was ready

- **Wrong:** the loader sent the identity query as soon as EspUsbHost reported the FM-1 connected.
- **Caught by:** checkpoint 1's first attempt: "couldn't send to the FM-1". The library reports a device connected
  before its interfaces are claimed.
- **Changed:** wait for MIDI-ready and broker ownership (`e848423`).

## 4. A real fix that wasn't the fix

- **Wrong:** at checkpoint 2 the board rebooted every ~3 s. `18b5de5` found and fixed allocation inside a critical
  section, a real abort.
- **Caught by:** the reboots continuing. `231af19` added breadcrumbs and safe mode because they did; then the first
  decoded core dump showed the loop task's stack overflowing in LVGL's redraw (`b81773c`).
- **Lesson:** a plausible cause, fixed, isn't proof it was *the* cause. The project built crash forensics instead
  of guessing again.

## 5. Parsing a file system by byte offsets

- **Wrong:** `store-add` read the store's littlefs superblock by fixed offsets.
- **Caught by:** the board's own store. It had upgraded itself to littlefs disk version 2.1, the parse read text as
  geometry, and `store-add` refused it.
- **Changed:** find the version by mounting: the oldest version that mounts is the image's own (`477b484`).

## 6. A screen line that guessed

- **Wrong:** a device whose MIDI didn't open was labelled "It may not be class-compliant".
- **Caught by:** a Launchkey, perfectly class-compliant, whose MIDI simply didn't reopen after many host restarts.
- **Changed:** the fallback no longer guesses (`605c522`).

## 7. A hold labelled "Back up" ran an install

The most serious one, because it reached the FM-1.
- **What happened** (checkpoint 3, second session): the board booted with the FM-1 in ROM boot and found the first
  attempt's pending install. The top row's hold had become "retry the install", but the row still read "Back up this
  FM-1". Chad held it as a backup. It ran the first attempt's confirmed install plan (fresh start, 143 + 6 sectors)
  as a resume.
- **Damage:** none lasting. The install was one Chad had confirmed earlier, it verified, and the Skiff saves stayed
  in the store. But the hold did something its label didn't say.
- **Caught by:** the result, which named an install where a backup was expected. The record doesn't say who
  noticed first.
- **Changed:** the row reads "Retry the … install", `/status` shows the top row, and the agent quotes it and the
  preview to Chad before every hold (`b4a4d71`). The hold runs only the package and unit the preview was computed
  for.

## 8. A stale pending record blocked backups

- **Wrong:** a pending record left by an install that stopped before writing made the top row a retry, even with the
  FM-1 running Skiff normally. Backups were unreachable.
- **Caught by:** checkpoint 3's first attempt.
- **Changed:** the retry is offered only while the FM-1 isn't back as MIDI (`904f39e`), and a run that stops before
  any erase clears its record (`d860144`).

## 9. A design that didn't know the FM-1's clock

- **Wrong:** the install took its backup inside ROM boot (two full reads), then planned, then erased.
- **Caught by:** checkpoint 3, leg 2, twice: the FM-1 left ROM boot about 8.5 s after the loader started, before
  the first erase.
- **Changed:** plan from the stored backup before the key; one read in ROM boot; a 6 s budget to the first erase
  (`d860144`). The cause of the 8.5 s is still a hypothesis.

## 10. Restarting the host in place

- **Wrong:** the architecture said changing mode means `usb.end()` then `usb.begin()`. The workarounds stacked up:
  restart the host when the ROM device isn't claimed (`536ee16`), restart it when none shows up (`d22c2db`), stop hub
  polls near a restart (`6b2fbfb`).
- **Caught by:** checkpoint 3's logs. Every USB failure came after an in-place restart, and the first job after a
  fresh boot always worked.
- **Changed:** one boot per job (`74033a9`). The workarounds went with it.

## 11. Suspects named as causes

- **Wrong:** hub port polls were taken out of loader builds as the likely trigger of the `hcd_urb_dequeue` crash
  (`3242075`). Later hub tracking was turned off in job boots for the same reason (`5272934`).
- **Caught by:** the crash continuing after each change, in later checkpoint holds and in the soak.
- **Where it stands:** both changes cut the rate; neither is shown to be the cause. The root cause is **open**.

## 12. "0 crashes", after a crash

- **Wrong:** `fc72748`'s commit message reports the hub-tracking-off soak as "7 runs, 0 crashes (the baseline had 2
  in 6)".
- **Caught by:** the same soak's own records. Run 8's first attempt crashed with the same assert at 10:23:22 on
  2026-10-30, and `soak_watch.py` saved its crash record then. The commit is timestamped 10:28:48 the same morning,
  so the message was wrong when it was written, not merely early. Run 8's retry boot passed.
- **Changed:** the plan records the crash (`b6de912`), and "fixed" now needs at least 20 soak runs with 0 crashes.

### The soak counts

The written record disagrees with the soak logs (`build/diag/soak-*/log.txt`, kept on the Mac, not in git) and with
itself:

| Source | `a616453` baseline | `77ff4ec`, hub tracking off |
|---|---|---|
| `5272934` | 6 runs: 3 ok, 2 ROM devices never seen, 2 crashes | |
| `fc72748` | | 7 runs, 0 crashes, 2 restarted into firmware |
| Plan (`b6de912`) | as `5272934` | 9 runs: 7 ok, 1 crash, 2 restarted |
| **The logs** | **6 finished runs: 3 ok, 3 with no ROM device after two boots. 2 crashes, each inside a run that then failed.** | **8 finished runs: 6 ok, 2 with no ROM device. 1 crash, inside run 8, whose retry passed. Then a 9th run (below).** |

How to read them: the soak counts run outcomes and crashes separately, and a crash can sit inside a run that later
passes. Under that convention the plan's 9 runs fit if the 9th counts as ok. In the log, the 9th run's backup
reached its last step and the ROM device left; then nothing was logged for about 107 s (no FM-1 back as MIDI, no
job end) until a power-on reset. Whether it finished is unknown. The baseline line in `5272934` doesn't match its
log under either reading: the log has 3 failed runs, 2 of them after crashes. These pages use the logs' numbers.

## 13. A finished install didn't update the backup

- **Wrong:** after a verified install, the unit's latest backup still described the old firmware. The page marked
  Skiff running after Skiff → Felucca, and the next preview planned from the old flash.
- **Caught by:** the stale-backup check, the safety net built for exactly this. A second Felucca hold was refused
  before any erase: "the FM-1 changed since its backup".
- **Changed:** a verified install records the written flash as the unit's latest backup (`c22ba65`).

## 14. A page that didn't fit

- **Wrong:** the Firmware page grew a package list that doesn't fit 240×280. On the board it scrolled, and a
  scrolling page loses holds to finger drift.
- **Caught by:** `make ui-shots`: 93 problems, all on the hub-loader Firmware page, which blocks CI.
- **Where it stands:** `154d147` on `loader-session3` (after this page's base) splits it into a list and a detail
  screen. Not checked here.

## 15. Review counts that don't add up

Several merge commits' review lines have more fixed and dismissed findings than critical plus important ones, for
example `e2da15a`: "3 important (7 fixed, 1 dismissed)". The difference is probably lower-severity findings, but the
lines don't say. See the [review table](robustness.md#layer-7-code-review).

## What the list says

- **Hardware caught most of them** (1, 3, 4, 5, 6, 7, 8, 9, 10). The board is the test that matters for timing,
  stacks and the FM-1's own behaviour.
- **The reference tools caught the protocol mistake** (2) before it reached hardware.
- **The safety design caught its own gap** (13): the stale-backup check refused a hold built on old data.
- **The record itself had errors** (12, 15). A commit message reported 0 crashes after a crash had been recorded,
  and the soak counts disagree from one document to the next. For a public claim, go back to the logs.
- **The one that reached the FM-1 unintended** (7) was a UI label, not flash logic. The fix was partly code and
  partly process: read the screen's state over Wi-Fi and say it out loud before every hold.
