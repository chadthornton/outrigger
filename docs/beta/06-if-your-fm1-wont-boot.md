# If your FM-1 won't boot

An FM-1 that won't start after a firmware switch can almost always be brought back. The order matters, so
go step by step.

## 1. If the board says the install stopped: don't switch the FM-1 off

If the board's screen says **Switch stopped** or **Install unfinished**, with "Don't unplug the FM-1 or switch it
off":

1. **Leave the FM-1 switched on, and leave every cable where it is.** Its screen is dark because it's still in
   install mode. That's what lets the board finish the job.
2. If something came unplugged, plug it back in.
3. On the board, hold the red **Hold: retry …** button for two seconds. It carries on where it stopped.
4. If Retry fails twice, go to step 3 below. **Still don't switch the FM-1 off.**

Switching the FM-1 off at this point makes things harder: with half a firmware, it can't go back into install mode
by itself, and you'd need the recovery kit below.

## 2. If the FM-1 is dark but the board says the switch finished

1. Switch the FM-1 off and on again.
2. If it starts but crashes, or its presets look scrambled, switch back to the firmware you had before, on the
   board's **FM-1** page. Your songs for that firmware come back with it. After a switch to M-VAVE's stock
   firmware, **System > Reset Patches** on the FM-1 often fixes scrambled presets.
3. If it still shows nothing at all, go to step 3.

## 3. Tell us, before trying anything else

Email **feedback@outrigger.music** right away with:

- a photo of the board's screen;
- the build number (the board's **Setup** screen);
- which firmware you were switching from and to.

**Please don't try other tools or web updaters first.** Some of them talk to the FM-1's normal firmware over
MIDI. On a half-written FM-1 they fail at best, and at worst leave it harder to recover.

## 4. The recovery kit: when nothing else works

When an FM-1 shows nothing at all, a small kit can force it into install mode at power-on, whatever state its
firmware is in. Then a recovery tool writes M-VAVE's stock firmware back. It brought a fully dark FM-1 back on
2026-10-25.

**The parts,** about $15, no soldering:

- **Seeed XIAO RP2040 with pre-soldered headers.** It must be the RP2040, not the RP2350.
- **A USB-C to 4-pin header cable.** A "USB-C to 4 pin header motherboard adapter cable" with separate 2.54 mm
  sockets works.
- **A USB-C data cable** from the XIAO to your computer.

**What the recovery looks like:**

1. Three wires go from the XIAO to the FM-1's USB-C port: data+, data− and ground. The fourth wire, power, is
   taped off. The FM-1 is switched off while you wire it.
2. The recovery tool reads the FM-1 twice to make a backup, checks the two reads match, then writes only the
   firmware area and reads it back.
3. Unwire it, switch the FM-1 on, and it starts on M-VAVE's stock firmware. Then switch to your firmware again
   from the board.

We'll walk you through it step by step. **We'd rather you had this kit before you start testing firmware
switches.** If you don't have one, tell us before your first switch.

The tools behind it, for the curious:
[FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) (the XIAO side) and
[MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) (the recovery tool).
