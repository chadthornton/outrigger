# Open questions

[Index](index.md) · previous: [Mistakes, and how each was caught](mistakes.md)

Things this project saw but hasn't explained, as of 2026-10-30. Each one says what's known, what would settle it,
and whether it blocks anything. Where the other pages say **open**, it's listed here.

## Blocking writes on by default

**Why ESP-IDF asserts in `hcd_urb_dequeue` at the FM-1's disconnect after the key.**
- **Known:** `hcd_dwc.c:2661`, on the USB client task, within about 1 s of the FM-1 leaving USB for ROM boot, never
  during a write. 2 crashes in 6 soak runs with hub tracking on; 1 in 8 with it off in job boots. Core dumps are kept.
- **Suspects:** EspUsbHost's disconnect teardown with MIDI OUT transfers still in flight; app hub requests colliding
  with ESP-IDF's hub driver on EP0.
- **Settles it:** a fix, then a soak of at least 20 runs with 0 crashes.
- **Detail:** [USB failures, section 7](usb-host-failures.md#7-the-hcd_urb_dequeue-assert-open-blocks-shipping-writes).

## The FM-1

- **What ends ROM boot about 8.5 s after the loader starts when no erase comes.** Hypothesis: a loader watchdog that
  erases and writes feed and reads don't. Settles it: time a backup that stops reading for 9 s. The design works
  around it with a 6 s budget either way.
- **What SCSI inquiry reports once the loader is running.** The resume path asks the loader (`online_device`)
  instead, so this no longer blocks resume.
- **The loader's USB buffer size** (`0xFC14`). The mock assumes 512.
- **Why the first checkpoint 3 attempt stopped** with "the FM-1 stopped answering", and why the next key brought up
  no ROM device until a power cycle. Suspects: the battery under erase current, or a loader or USB fault. Nothing
  was written.
- **Stock V15's data map.** Needs a stock unit's flash dumped before and after changing settings. Until then stock
  saves stay off.
- **SLOOP and other Felucca 0.9 firmwares:** whether they keep data outside the Felucca table. Needs store dumps
  diffed before SLOOP is installed over Skiff data or the reverse.
- **Whether factory voices are identical across units.** If they are, a second unit's backup costs less. Needs
  Chad's second FM-1.
- **Felucca's own backup protocol over USB-MIDI** (`fm1backup.js`, used by Melodee) could make saves without a
  restart. Unverified.

## The USB host

- **Why `usb.end()`/`usb.begin()` leaves the host unreliable** (asserts, missed devices, no re-enumeration). Job boots
  avoid it, so it doesn't block anything now, but a Play ↔ Record switch would want it.
- **Why the ROM device was missed twice:** once while the broker still seemed to count the departed FM-1
  (`536ee16`), once when a hub event went unseen and the device appeared the instant the host restarted
  (`d22c2db`).
- **Whether a device whose interfaces are all refused still holds its EP0 channel.** The broker assumes yes.
- **Why the hub's inner chip never enables,** which makes its USB-A ports useless on the Waveshare.

## The board

- **The first HTTP answer after a boot often takes about 20 s,** though the log shows Wi-Fi online at about 5.5 s.
  A probe (`tools/boot_probe.py` on branch `boot-probe`) is written; no board result is recorded yet.
- **What happened in the last soak run.** Its backup reached the FM-1's reset and the ROM device left, then nothing
  was logged for about 107 s until a power-on reset. A hang would look the same as a quiet finish.
- **Wi-Fi and BLE together** (coexistence) is unmeasured, so the design allows one radio client at a time.

## Not questions, but not done

- **The review round** over `loader-session3`'s guarded work since `d2284ba`.
- **Store packing:** a file per 4 KiB sector costs about 2.6× space.
- **Decrypting before sharing sectors** might beat the 1.41× measured on encrypted bytes. Unmeasured.
