# mdx2wav for the browser (WebAssembly)

This directory contains a WebAssembly port of mdx2wav: the original C++ engine
(MXDRVG + fmgen YM2151 + PCM8 + the SDL2 visualizer) compiled with Emscripten and
driven from JavaScript, so an MDX file plays with both sound *and* the animated
visualizer in a normal web page.

The result is bit-for-bit identical to the native `mdx2wav` output for the first
seconds of playback (see "Verification" below), and the engine runs about 21x
faster than real time.

```
open http://127.0.0.1:8099/   →  MHAWK3.MDX plays with the visualizer
```

## Quick start

```shell
# 1. build the engine (needs the toolchain in tools/, see below)
./web/build.sh

# 2. serve web/dist/ with the COOP/COEP headers SharedArrayBuffer requires
node web/serve.js                       # http://127.0.0.1:8099/

# 3. optional: automated end-to-end check in a real browser
node web/verify.js
```

`web/serve.js` serves the built files, the AudioWorklet and the MDX/PDX files.
By default it looks for songs in `../../mdx/metalhawk` (i.e.
`/Users/<you>/mdx/metalhawk`); override with `--songs /path/to/mdx/dir`. The page
lets you pick any of the MDX files it finds, or open a local `.mdx`/`.pdx` pair.

## What was ported

| Piece | Source | Notes |
| --- | --- | --- |
| MDX driver | `gamdx/mxdrvg` | unchanged, reused as-is |
| YM2151 emulation | `gamdx/fmgen`, `gamdx/mame` | fmgen by default |
| ADPCM / PCM8 | `gamdx/pcm8` | unchanged |
| Resampler | `gamdx/downsample` | unchanged |
| Visualizer | `src/visualizer` | reused through Emscripten's SDL2 / SDL2_ttf / SDL2_image ports |
| Harness | `web/src/mdxweb.cpp` | new: step-driven loop replacing `src/mdx2wav.cpp`'s `main()` |
| MDX/PDX loading | `web/src/mdxweb_load.cpp` | new: browser equivalent of `LoadMDX()` |
| FFmpeg video encoder | `web/src/video_encoder_stub.cpp` | stubbed out; the browser draws to a canvas instead |

Audio is generated 10 ms at a time (`mdxweb_step()`), copied into a
`SharedArrayBuffer` ring and drained by an `AudioWorklet`. A visualizer frame is
rendered every seventh step (~14 Hz) so that drawing never throttles audio
generation. All engine code runs on the Emscripten main thread, which is what
SDL2 needs to own the canvas.

## Building

The toolchain lives in `tools/` and is **not** part of the repository (it is
~1 GB). Install it once:

* `tools/upstream/` – Emscripten 6.0.9 + LLVM/binaryen
  (`wasm-binaries-arm64.tar.xz` from the official
  `webassembly/emscripten-releases-builds` bucket, extracted with the extra
  `install/` level removed)
* `tools/node/bin/node` – the Node that `emcc` drives
* `tools/pylocal/python/bin/python3` – Python 3.10+ (Emscripten rejects 3.9,
  which is what macOS ships)
* `tools/bin/emcc`, `tools/bin/em++` – wrappers that point Emscripten at the
  local Python and Node
* `tools/emscripten.config` – `LLVM_ROOT` / `BINARY_ROOT` / `NODE_JS` / `CACHE`
* optional: `tools/chrome/chrome-headless-shell-mac-arm64/` for `verify.js`

Then:

```shell
./web/build.sh        # writes web/dist/{mdxweb.js,mdxweb.wasm,mdxweb.data}
```

### Compiler flags worth knowing

* **`-O2` is the default.** The engine runs at roughly 21× real time on a laptop
  at `-O2` (about 7× at `-O0`). Reaching `-O2` required fixing real undefined
  behaviour in the shared sources — see "Source changes outside `web/`" below;
  before those fixes the optimiser turned it into a trapping `unreachable`.
* `-sSTACK_SIZE` / `-sDEFAULT_PTHREAD_STACK_SIZE` are set to 32 MB. The
  visualiser keeps large `YM2151State::Channel` arrays on the stack.
* `-pthread` + `-sPTHREAD_POOL_SIZE=8`. Emscripten needs a worker thread to run
  `main` so that SDL2 can own the browser main thread.

## Source changes outside `web/`

Three fixes were made to the shared sources. They are plain bug fixes, not
browser-only workarounds, and the native build is unaffected.

* `src/visualizer/visualizer.cpp` — `getColorFromAddress()`
  * **This is what blocked `-O1`/`-O2`.** It computed
    `address ^ (address >> 32)`, but `uintptr_t` is 32 bits wide on wasm32, so
    that shift is by the full width of the type — undefined behaviour. clang at
    `-O1`/`-O2` replaces the function with `unreachable`, so the engine trapped
    as soon as an ADPCM channel with a non-zero address was drawn. The shift is
    now only done when `sizeof(uintptr_t) > 4`; the high half is zero on 32-bit
    targets anyway, and 64-bit builds (the native binary) keep the exact
    original colours.
  * `r1`/`g1`/`b1` are also initialised before the hue branch, so the
    `(r1 + m)` arithmetic can never read an indeterminate value.
* `gamdx/mxdrvg/mxdrvg_core.h` — `L0005f8()` (the driver's block copy)
  * the loop casts the source and destination to `uint32_t *` and dereferences
    them. The 68000 tolerates unaligned longword access and x86 tolerates it
    too, but the C is undefined behaviour and traps on WebAssembly. It now falls
    back to a byte copy when either pointer is not 4-byte aligned; every buffer
    the driver normally uses is aligned, so the fast path is unaffected.

## Files

```
web/
  build.sh                  build engine + page into web/dist/
  build_native_test.sh      build web/test/native_harness_test (host, no SDL)
  serve.js                  dev server (COOP/COEP), song list, worklet
  verify.js                 headless-browser end-to-end verification
  index.html, app.js        the page: canvas, controls, ring-buffer pump
  worklet/player-processor.js   AudioWorklet that drains the ring
  src/mdxweb.cpp            engine harness / exported API
  src/mdxweb_load.{h,cpp}   MDX + PDX loading, shared with the native test
  src/video_encoder_stub.cpp  FFmpeg encoder stub
  src/pre.js                Module setup, runs before the runtime
  test/native_harness_test.cpp  driver for mdxweb_load_data() on the host
  tools/cdp.js              tiny Chrome DevTools Protocol driver (no npm deps)
  tools/compare_pcm.py      PCM comparison with time-offset compensation
  dist/                     build output (mdxweb.js/.wasm/.data)
```

## Verification

`node web/verify.js` does all of the following in a real (headless) browser:

1. boots the engine and reports the song title and measured duration,
2. plays for N seconds and checks that audio is produced, that the ring never
   underruns and that the audio clock keeps up with wall time,
3. writes a screenshot of the canvas to `web/verify-out/screenshot.png`,
4. renders 3 s of PCM on a **fresh page** and compares it sample by sample with
   the output of the native `build/mdx2wav` binary.

For MHAWK3.MDX the comparison currently reports:

```
compensated 0 ms start offset
compared 176383 samples (88191 stereo frames)
difference RMS: 0.00 (0.000% of reference)
differing samples: 0 (0.00%), max delta 0
OK: web output matches the native build
==> VERIFY OK
```

The same DSP path can be exercised without a browser, which is much faster when
debugging the loader:

```shell
./web/build_native_test.sh
./web/build/native_harness_test ../../mdx/metalhawk/MHAWK3.MDX \
    ../../mdx/metalhawk/MHAWK.PDX 132300 /tmp/harness.pcm
python3 web/tools/compare_pcm.py /tmp/harness.pcm /path/to/native.pcm
```

### Why the loader looks the way it does

MXDRVG's `MXDRVG_SetData()` is handed a pointer into the song data and then reads
a 10-byte driver header *before* it (the native loader gets this layout for free
because `read_file()` allocates `size + 10` and reads the file at offset 10).
`mdxweb_load_data()` reproduces that layout explicitly:

```
base
 ├─ pad (kHeadroom = 34 bytes, keeps the body pointer 4-byte aligned)
 ├─ 10-byte driver header      ← the pointer handed to MXDRVG_SetData()
 └─ file contents              ← the song body lives inside these
```

`drive_ptr = file + pos - MAGIC_OFFSET` is where the header is written and the
same pointer is passed to the driver; `pos` is the file-relative index of the
song body, found by the same parse the native loader performs. The buffers also
carry 64 KB of slack past the end, because the driver's copy routine can read
slightly past the end of the source (again invisible on the native build, which
gets it from malloc rounding).

## Known limitations

* The page must be served with `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`, otherwise `SharedArrayBuffer` is
  unavailable and the page reports an error. `web/serve.js` sets these.
* Browser autoplay rules require a user gesture before sound starts; the page
  shows a "click to start" overlay for this.
* The video-recording (`--video`), screenshot and spectrum-debug options of the
  native tool are not exposed in the browser; FFmpeg is stubbed out.
* The native binary is not bit-reproducible across runs: a handful of samples
  (~5 of 88200 in the first second) depend on uninitialised heap contents. The
  browser build starts from zeroed buffers, so its output is deterministic — and
  is what the comparison in `web/verify.js` is anchored to.
