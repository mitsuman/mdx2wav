# snes_spc 0.9.0 (vendored)

Blargg's SNES SPC-700 APU emulator, used by mdx2wav to play `.spc` files.

* Upstream: <https://github.com/blarggs-audio-libraries/snes_spc> (tag `0.9.0`)
* Author: Shay Green (Blargg)
* License: GNU Lesser General Public License, version 2.1 or later. See
  `LICENSE` / `license.txt` in this directory.

## Local changes

The only changes are additive accessors used by the visualizer:

* `SPC_DSP::voices()`, `SPC_DSP::regs()`, `SPC_DSP::echo_offset()` and
  `SPC_DSP::echo_length()` expose read-only DSP state.
* `SPC_DSP::dbg_waveform()` / `dbg_waveform_pos()` expose a small ring buffer
  of the actual per-voice output, captured after envelope and L/R volume in
  `voice_output()`.  This is what the visualizer draws.
* `SNES_SPC::dsp_state()`, `SNES_SPC::smp_ram()` and `SNES_SPC::cpu_pc()` expose
  the DSP object, APU RAM and CPU program counter.
* The two `(~0x2F00 << …)` register tests were changed to unsigned arithmetic.
  The original expression shifts a negative `int`, which is undefined behaviour
  and was diagnosed by clang when compiled for WebAssembly; the comparison's
  value is unchanged on two's-complement targets.

The emulation itself is untouched.  mdx2wav links these sources statically; the
complete corresponding source is included in this repository, so the LGPL's
relinking requirement is satisfied.  The rest of mdx2wav remains Apache-2.0.

`SNES_SPC_state.cpp` is required by the build even if state save/load is not
used, because `SNES_SPC::copy_state` is compiled unconditionally.
