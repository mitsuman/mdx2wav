# mdx2wav

mdx2wav is a converter to make raw pcm data from MDX file on Linux.

MDX is a major sound format of Sharp X68000 in the 1990s.

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

### Options

```shell
  -d <sec>  : limit song duration. 0 means nolimit. (default:300)
  -e <type> : set ym2151 emulation type, fmgen or mame. (default:fmgen)
  -f        : enable fadeout.
  -l <loop> : set loop limit. (default:2)
  -m        : measure play time as sec.
  -r <rate> : set sampling rate. (default:44100)
  -t        : get song title (charset is SHIFT-JIS).
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
