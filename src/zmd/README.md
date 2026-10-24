# Z-MUSIC (ZMD) C++ port

This directory contains a C++ port of the FM playback part of **Z-MUSIC
SYSTEM Ver.2**, so that `mdx2wav` can render `.zmd` files directly.

* Original Z-MUSIC author: Zenji Nishikawa (西川善司)
* Reference source: <https://github.com/kg68k/zmusic2> (`unified/`, Ver.2.08)
* Main reference files: `unified/zmusic.s`, `unified/interrupt.s`,
  `unified/work.s`, and the ZMD format chapter of the manual (`manual/zm12.txt`).

The original source is published by the author for unrestricted use; see the
license section of the zmusic2 README.

## Files

| File | Purpose |
|---|---|
| `zmd_player.h` | `ZmdPlayer` interface, per-track state |
| `zmd_player.cpp` | ZMD parser, sequencer, YM2151 register layer |
| `../mdx2wav.cpp` | `.zmd` detection, CLI path (`run_zmd_file`) |

## Signal path

1. Parse `$10 "ZmuSiC" version`, then the common-command stream up to `$FF`.
   Voice banks (`$04`/`$1B`), tempo (`$05`), master clock (`$42`), comments
   (`$7F`) and ADPCM configuration (`$40`) are recognized.
2. Read the track table.  All words are big-endian.  A track offset is relative
   to the address just after the 4-byte offset field, as in the original
   driver (`start = entry + 4 + offset`).
3. Each FM track is a small state machine.  One sequencer tick is the OPM
   timer-A period:
   `tick = 16e-6 * floor(78125 / tempo)` seconds.
   Step and gate times are counted in these ticks.
4. ZMD note numbers go through the Z-MUSIC key-code table (`kKcTable`) and the
   fine-detune value (`D1`) before being written to KC/KF.
5. A 55-byte Z-MUSIC voice is translated to YM2151 registers:
   AF/pan (`$20`), LFO (`$18`–`$1B`, `$38`), DT1/MUL, TL, KS/AR, AMS/D1R,
   DT2/D2R, D1L/RR for each operator.  Carrier operators get
   `voice TL + volume`; modulators keep their voice TL.
6. Audio comes from the existing fmgen YM2151 core at 4 MHz.

## Commands implemented

Notes/rests, tie (`gate = 255`), tempo (`$90`/`$91`, relative `$92`–`$95`),
detune/transpose (`$D1`, `$96`/`$97`), voice change (`$A0`/`$A1`),
pan (`$B0`–`$B4`), volume (`$B6`, velocity `$B9`, relative `$AA`/`$AB`),
direct OPM write (`$B5`), pitch/amplitude LFO (`$D6`, `$E6`, `$E8`, `$AE`,
waveform `$98`), repeats (`$C1`/`$C2`), conditional endings (`$C3`/`$C4`),
`DO`/`LOOP` (`$C0` 9/10), and the `$FE` absolute-length note.

ADPCM: ZPD loading (`$40`), playback (`$9B`), frequency (`$A2`/`$A9`) and pan
(`$B7`), on channel 8.  ZPD data is decoded at 15.6 kHz with the MSM6258 step
table and resampled to the output rate; registration volume/pitch, cut offset/
size and reverse are applied.  Fade and mix processing are not implemented.

The 14 Metal Unit II tracks use this subset exactly: notes/rests, `$91`,
`$A0`, `$B1`–`$B3`, `$B6`, `$C0`–`$C4`, `$D1`, `$D6`, `$E6`, `$E8`, `$AE`.
They contain no ADPCM commands.

## Known differences from the 68000 driver

* The software LFO is a simplified triangle/square/saw generator; its exact
  phase/quarter-period behaviour is not bit-identical to `pmod_ope` /
  `amod_ope`.
* ADPCM supports one channel and ZPD files.  P16/PCM8 sample formats,
  registration-time mix/fade processing and per-channel PCM8 (8 ADPCM voices)
  are not implemented.
* `D.C.` / `D.S.` / `CODA` / `FINE` flow commands are not fully implemented
  (the target files only use `DO`/`LOOP`).
* MIDI, PCM8, RS-MIDI, portamento, auto-bend, chord and after-touch commands
  are ignored.
* Tie/dumper handling assumes one voice per FM track, which is how the target
  files are written (no `E2` chords).
* `-p` and the SDL2 visualizer are not connected to the ZMD engine.

## Verification

* Synthetic ZMD test: C4 renders at 261.1 Hz; left/right/centre pan and
  `DO`/`LOOP` behave as expected.
* Synthetic ZPD test: a 440 Hz MSM6258 sample plays at 440 Hz at 44.1/48 kHz,
  220 Hz at frequency setting 2 and at pitch −12, with correct L/R pan and
  volume scaling; a missing ZPD falls back to FM with a warning.
* All 14 tracks render without errors; measured `-l 2 -f` lengths are
  13.3 s to 214.1 s.  A 0.9 output gain keeps the loudest song
  (`Mu2_dofight`) below full scale.
