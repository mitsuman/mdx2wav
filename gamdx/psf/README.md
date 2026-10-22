# PSF playback core

This directory contains the code used to play PlayStation `.psf` / `.minipsf`
files.

- `psflib.c`, `psflib.h` — PSF container parser and `_lib` resolver by
  Christopher Snowhill, MIT licensed (see `LICENSE`).
- `he/` — the PS1/PS2 IOP emulation core from Neill Corlett's *Highly
  Experimental* (R3000 CPU, SPU, timers, virtual file I/O).  The original
  project has no license file; the code is PCSX-derived and is distributed as
  part of the GPLv2 Highly Experimental plugin.  Treat it as **GPLv2**: a
  binary built with this directory is a GPLv2 work, even though the rest of
  mdx2wav is Apache-2.0.

## BIOS requirement

PSF files contain PlayStation code, so playback needs a PS1 BIOS image.  The
original core expects the patched "Highly Experimental" BIOS; mdx2wav instead
uses a stock BIOS and stops its early boot loop after a bounded number of
cycles (`preboot()` in `he/psx.c`), which leaves the kernel tables and the SPU
driver initialised — enough for PSF sound drivers.

The BIOS image is **not** included.  Supply your own dump with `--psf-bios`,
`$PSF_BIOS`, `./psx-bios.bin` or `~/.config/mdx2wav/psx-bios.bin`.

## Local changes

- `he/psx.c`: `preboot()` gives up after ~30M cycles so a stock BIOS can be
  used.
- `he/spu.h`, `he/spu.c`: added `spu_getreg_voice()`, `spu_getreg_main()` and
  `spu_getreg_flag()` so the visualizer can read the SPU voice registers.
- `he/r3000asm.c` is not built (only needed by the original debug tools).
