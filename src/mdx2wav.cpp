#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamdx/mxdrvg/mxdrvg.h"

#define VERSION "1.0"

bool verbose = false;

typedef unsigned char u8;
typedef unsigned int u32;

const int MAGIC_OFFSET = 10;

// @param mode 0:tolower, 1:toupper, 2:normal
void strcpy_cnv(char *dst, const char *src, int mode) {
  while (int c = *src++) {
    *dst++ =
      mode == 0 ? tolower(c) :
      mode == 1 ? toupper(c) :
      c;
  }
  *dst = 0;
}

bool read_file(const char *name, int *fsize, u8 **fdata, int offset) {
  *fdata = 0;
  *fsize = 0;

  int fd = open(name, O_RDONLY);
  if (fd == -1) {
    if (verbose) {
      fprintf(stderr, "cannot open %s\n", name);
    }
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) == -1) {
    if (verbose) {
      fprintf(stderr, "cannot fstat %s\n", name);
    }
    st.st_size = 128 * 1024; // set tentative file size
  }

  int size = st.st_size;
  if (size == 0) {
    fprintf(stderr, "Invalid file size %s\n", name);
    close(fd);
    return false;
  }

  u8 *data = new u8[size + offset];
  size = read(fd, data + offset, size);

  close(fd);

  *fdata = data;
  *fsize = size + offset;
  return true;
}

bool LoadMDX(const char *mdx_name, char *title, int title_len) {
  u8 *mdx_buf = 0, *pdx_buf = 0;
  int mdx_size = 0, pdx_size = 0;

  // Load MDX file
  if (!read_file(mdx_name, &mdx_size, &mdx_buf, MAGIC_OFFSET)) {
    fprintf(stderr, "Cannot open/read %s.\n", mdx_name);
    return false;
  }

  // Skip title.
  int pos = MAGIC_OFFSET;
  {
    char *ptitle = title;
    while (pos < mdx_size && --title_len > 0) {
      *ptitle++ = mdx_buf[pos];
      if (mdx_buf[pos] == 0x0d && mdx_buf[pos + 1] == 0x0a)
        break;
      pos++;
    }
    *ptitle = 0;
  }

  while (pos < mdx_size) {
    u8 c = mdx_buf[pos++];
    if (c == 0x1a) break;
  }

  char *pdx_name = (char*) mdx_buf + pos;

  while (pos < mdx_size) {
    u8 c = mdx_buf[pos++];
    if (c == 0) break;
  }

  if (pos >= mdx_size)
    return false;

  // Get mdx path.
  if (*pdx_name) {
    char pdx_path[FILENAME_MAX];
    strncpy(pdx_path, mdx_name, sizeof(pdx_path));

    int pdx_name_start = 0;
    for (int i = strlen(pdx_path) - 1; i > 0; i--) {
      if (pdx_path[i - 1] == '/') {
        pdx_name_start = i;
        break;
      }
    }

    if (pdx_name_start + strlen(pdx_path) + 4 >= sizeof(pdx_path)) {
      return false;
    }

    // remove .pdx from pdx_name
    {
      int pdx_name_len = strlen(pdx_name);
      if (pdx_name_len > 4) {
        if (pdx_name[pdx_name_len - 4] == '.') {
          pdx_name[pdx_name_len - 4] = 0;
        }
      }
    }

    // Make pdx path.
    for (int i = 0; i < 3 * 2; i++) {
      strcpy_cnv(pdx_path + pdx_name_start, pdx_name, i % 3);
      strcpy_cnv(pdx_path + pdx_name_start + strlen(pdx_name), ".pdx", i / 3);
      if (verbose) {
        fprintf(stderr, "try to open pdx:%s\n", pdx_path);
      }
      if (read_file(pdx_path, &pdx_size, &pdx_buf, MAGIC_OFFSET)) {
        break;
      }
    }
  }


  // Convert mdx to MXDRVG readable structure.
  int mdx_body_pos = pos;

  if (verbose) {
    fprintf(stderr, "mdx body pos  :0x%x\n", mdx_body_pos - MAGIC_OFFSET);
    fprintf(stderr, "mdx body size :0x%x\n", mdx_size - mdx_body_pos - MAGIC_OFFSET);
  }

  u8 *mdx_head = mdx_buf + mdx_body_pos - MAGIC_OFFSET;
  mdx_head[0] = 0x00;
  mdx_head[1] = 0x00;
  mdx_head[2] = (pdx_buf ? 0 : 0xff);
  mdx_head[3] = (pdx_buf ? 0 : 0xff);
  mdx_head[4] = 0;
  mdx_head[5] = 0x0a;
  mdx_head[6] = 0x00;
  mdx_head[7] = 0x08;
  mdx_head[8] = 0x00;
  mdx_head[9] = 0x00;

  if (pdx_buf) {
    pdx_buf[0] = 0x00;
    pdx_buf[1] = 0x00;
    pdx_buf[2] = 0x00;
    pdx_buf[3] = 0x00;
    pdx_buf[4] = 0x00;
    pdx_buf[5] = 0x0a;
    pdx_buf[6] = 0x00;
    pdx_buf[7] = 0x02;
    pdx_buf[8] = 0x00;
    pdx_buf[9] = 0x00;
  }

  if (verbose) {
    fprintf(stderr, "instrument pos:0x%x\n", mdx_body_pos - 10 + (mdx_head[10] << 8) + mdx_head[11]);
  }

  MXDRVG_SetData(mdx_head, mdx_size, pdx_buf, pdx_size);

  delete []mdx_buf;
  delete []pdx_buf;

  return true;
}

void version() {
  printf(
    "mdx2wav version "VERSION"\n"
    "Copyright 2014 @__mtm\n"
    " based on MDXDRVg V1.50a (C) 2000 GORRY.\n"
    "  converted from X68k MXDRV music driver version 2.06+17 Rel.X5-S\n"
    "   (c)1988-92 milk.,K.MAEKAWA, Missy.M, Yatsube\n"
    );
}

void help() {
  printf(
    "Usage: mdx2wav [options] <file>\n"
    "Convert mdx file to 16bit stereo raw pcm and write it to stdout.\n"
    " if you need to convert to other format, use ffmpeg like a following.\n"
    "  mdx2wav xxx.mdx | ffmpeg -f s16le -ar 44.1k -ac 2 -i - xxx.wav\n"
    " if you want to listen now, use aplay like a following.\n"
    "  mdx2wav xxx.mdx | aplay -f cd\n"
    "Options:\n"
    "  -d <sec>  : limit song duration. 0 means nolimit. (default:300)\n"
    "  -e <type> : set ym2151 emulation type, fmgen or mame. (default:fmgen)\n"
    //"  -f        : enable fadeout.\n"
    "  -l <loop> : set loop limit. (default:2)\n"
    "  -m        : measure play time as sec.\n"
    "  -r <rate> : set sampling rate. (default:44100)\n"
    "  -t        : get song title (charset is SHIFT-JIS).\n"
    "              if you need other charset, try following command:\n"
    "               mdx2wav -t xxx.mdx | iconv -f SHIFT-JIS -t utf-8\n"
    "  -v        : print version.\n"
    "  -V        : verbose, write debug log to stderr.\n"
    );
}



int main(int argc, char **argv) {
  int MDX_BUF_SIZE = 256 * 1024;
  int PDX_BUF_SIZE = 1024 * 1024;
  int SAMPLE_RATE = 44100;
  int filter_mode = 0;

  bool measure_play_time = false;
  bool get_title = false;
  float max_song_duration = 300.0f;
  int loop = 2;
  int fadeout = 0;
  char ym2151_type[8] = "fmgen";

  int opt;
  while ((opt = getopt(argc, argv, "d:e:fl:mr:tvV")) != -1) {
    switch (opt) {
      case 'd':
        max_song_duration = atof(optarg);
        break;
      case 'e':
        strncpy(ym2151_type, optarg, sizeof(ym2151_type));
        break;
      case 'f':
        fadeout = 1;
        break;
      case 'l':
        loop = atoi(optarg);
        break;
      case 'm':
        measure_play_time = true;
        break;
      case 'r':
        SAMPLE_RATE = atoi(optarg);
        break;
      case 't':
        get_title = true;
        break;
      case 'v':
        version();
        return 0;
      case 'V':
        verbose = true;
        break;
      default:
        help();
        return 0;
    }
  }

  int AUDIO_BUF_SAMPLES = SAMPLE_RATE / 100; // 10ms

  const char *mdx_name = argv[optind];
  if (mdx_name == 0 || *mdx_name == 0) {
    help();
    return 0;
  }

  if (0 == strcmp(ym2151_type, "fmgen")) {
  } else if (0 == strcmp(ym2151_type, "mame")) {
    MXDRVG_SetEmulationType(MXDRVG_YM2151TYPE_MAME);
  } else {
    fprintf(stderr, "Invalid ym2151 emulation type: %s.\n", ym2151_type);
    return -1;
  }

  MXDRVG_Start(SAMPLE_RATE, filter_mode, MDX_BUF_SIZE, PDX_BUF_SIZE);
  MXDRVG_TotalVolume(256);

  char title[256];

  if (!LoadMDX(mdx_name, title, sizeof(title))) {
    return -1;
  }

  if (get_title) {
    printf("%s\n", title);
    return 0;
  }

  float song_duration = MXDRVG_MeasurePlayTime(loop, fadeout) / 1000.0f;
  // Warning: MXDRVG_MeasurePlayTime calls MXDRVG_End internaly,
  //          thus we need to call MXDRVG_PlayAt due to reset playing status.
  MXDRVG_PlayAt(0, loop, fadeout);

  if (measure_play_time) {
    printf("%d\n", (int) ceilf(song_duration));
    return 0;
  }

  if (verbose) {
    fprintf(stderr, "loop:%d fadeout:%d song_duration:%f\n", loop, fadeout, song_duration);
  }

  if (max_song_duration < song_duration) {
    song_duration = max_song_duration;
  }

  short *audio_buf = new short [AUDIO_BUF_SAMPLES * 2];

  for (int i = 0; song_duration == 0.0f || 1.0f * i * AUDIO_BUF_SAMPLES / SAMPLE_RATE < song_duration; i++) {
    if (MXDRVG_GetTerminated()) {
      break;
    }

    int len = MXDRVG_GetPCM(audio_buf, AUDIO_BUF_SAMPLES);
    if (len <= 0) {
      break;
    }

    fwrite(audio_buf, len, 4, stdout);
  }

  MXDRVG_End();

  delete []audio_buf;

  if (verbose) {
    fprintf(stderr, "completed.\n");
  }
  return 0;
}
