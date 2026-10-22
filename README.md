# mdx2wav

mdx2wav is a converter to make raw pcm data from MDX and SPC music files.

MDX is a major sound format of Sharp X68000 in the 1990s.  SPC is the
SNES APU dump format, containing an SPC700 program and S-DSP state; it is
played with Blargg's snes_spc emulator.

## Usage

mdx2wav dumps 16bit stereo 44.1kHz pcm data to stdout, so you need to use it with other commands.

To Play now,
```shell
mdx2wav xxx.mdx | aplay -f cd
```

To convert to other format,
```shell
mdx2wav xxx.mdx | ffmpeg -f s16le -ar 44.1k -ac 2 -i - xxx.wav
mdx2wav xxx.mdx | ffmpeg -f s16le -ar 44.1k -ac 2 -i - -ab 192 -f ogg file.ogg
```

SPC files use the same pipeline:
```shell
mdx2wav song.spc | ffmpeg -f s16le -ar 44.1k -ac 2 -i - song.wav
mdx2wav -g song.spc                 # SDL2 visualizer (8 S-DSP voices)
mdx2wav -t song.spc                 # ID666 title
```
The S-DSP runs at 32 kHz and is band-limited upsampled to the requested rate
(44.1 kHz by default).  SPC has no reliable song-length field, so playback is
capped by `-d` (default 300 s); the ID666 "seconds until fadeout" value is used
when present.  `-l` is an MDX-only option because the SPC program controls its
own looping.

### Options

```shell
  -d <sec>  : limit song duration. 0 means nolimit. (default:300)
  -e <type> : set ym2151 emulation type, fmgen or mame. (default:fmgen)
  -f        : enable fadeout.
  -l <loop> : set loop limit. (default:2, MDX only)
  -m        : measure play time as sec.
  -r <rate> : set sampling rate. (default:44100)
  -t        : get song title (charset is SHIFT-JIS; SPC ID666 tags are raw).
              if you need other charset, try following command:
               mdx2wav -t xxx.mdx | iconv -f SHIFT-JIS -t utf-8
  -v        : print version.
  -V        : verbose, write debug log to stderr.

```

## Build

```shell
git clone https://github.com/mitsuman/mdx2wav.git
cd mdx2wav
mkdir Release
cd Release
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

## Browser (WebAssembly) port

The same engine also runs in a web browser, with sound and the SDL2 visualizer,
via Emscripten:

```shell
./web/build.sh            # build web/dist/
node web/serve.js         # play at http://127.0.0.1:8099/
node web/verify.js        # headless end-to-end check
```

See [web/README.md](web/README.md) for the build requirements and details. The
browser output is bit-exact with the native `mdx2wav` for MDX, and within one
LSB on a handful of samples for SPC, over the first seconds of playback.

## PSF support

PSF (PlayStation Sound Format, `.psf` / `.minipsf`) files contain PlayStation
code, so mdx2wav runs them on the R3000 + SPU core from *Highly Experimental*
(vendored in `gamdx/psf/`, see the README there).  PSF playback needs a
PlayStation BIOS image, which cannot be bundled: pass one with
`--psf-bios <file>`, or put it at `$PSF_BIOS`, `./psx-bios.bin` or
`~/.config/mdx2wav/psx-bios.bin`.  Song length and fade come from the
`length` / `fade` tags, and `-t` prints the title.

The PSF panel shows the 24 SPU voices in two columns (12 + 12) with their
pitch, L/R volume, envelope, ADSR settings and note, plus a compact five-octave
keyboard.  Notes are derived from the pitch register and the decoded VAG
sample's own tuning, so they are absolute rather than guessed from the mix.  Only PSF1 is supported (PSF2 would need a
PS2 emulator), and the web build does not include the PSF core yet.

Note that `gamdx/psf/he/` is GPLv2 (PCSX-derived), so a binary built with PSF
support is a GPLv2 work; the rest of the project stays Apache-2.0.

## SPC support

`gamdx/snes_spc/` contains Blargg's snes_spc 0.9.0 (LGPL-2.1-or-later) with
small read-only accessors added for the visualizer.  See
[gamdx/snes_spc/README.md](gamdx/snes_spc/README.md).  The SPC layout shows all
eight voices with note, pitch, envelope, source/loop addresses, the actual
per-voice DSP output waveform, keyboard, per-voice spectrum, and the global S-DSP status (master and
echo volumes, KON, directory, program counter).  On macOS, `-p` direct audio
playback works for SPC too; combine it with `-g` for sound and the visualizer.
