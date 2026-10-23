#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <algorithm>

#include "../gamdx/mxdrvg/mxdrvg.h"
#include "command_line_parser.h"
#include "spc/spc_player.h"
#include "psf/psf_player.h"

#ifdef __APPLE__
#include "platform_mac.h"
#endif

#ifdef ENABLE_VISUALIZER
#include "visualizer/visualizer.h"
#include "visualizer/ym2151_state.h"
#include "spc/spc_visualizer.h"
#include "../gamdx/mxdrvg/opm_visualizer.h"
#include "../gamdx/mxdrvg/pcm8_visualizer.h"
#include <pthread.h>
#endif

#define VERSION "1.0"

bool verbose = false;
bool g_continuous_playback = false;

#ifdef __APPLE__
// The AudioQueue callback runs on its own thread.  SPC state is shared with
// the visualizer, so both generation and state snapshots take this lock.
static SPCPlayer* g_spc_audio_player = nullptr;
static pthread_mutex_t g_spc_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static PSFPlayer* g_psf_audio_player = nullptr;
static pthread_mutex_t g_psf_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static int PsfAudioFill(short* out, int frames) {
  pthread_mutex_lock(&g_psf_audio_mutex);
  int len = 0;
  if (g_psf_audio_player && !g_psf_audio_player->IsFinished()) {
    len = g_psf_audio_player->Generate((int16_t*)out, frames);
  }
  pthread_mutex_unlock(&g_psf_audio_mutex);
  return len;
}

static int SpcAudioFill(short* out, int frames) {
  pthread_mutex_lock(&g_spc_audio_mutex);
  int len = 0;
  if (g_spc_audio_player && !g_spc_audio_player->IsFinished()) {
    len = g_spc_audio_player->Generate((int16_t*)out, frames);
  }
  if (verbose && len > 0) {
    static bool reported = false;
    if (!reported) {
      int peak = 0;
      for (int i = 0; i < len * 2; i++) {
        const int v = out[i] < 0 ? -out[i] : out[i];
        if (v > peak) peak = v;
      }
      fprintf(stderr, "SPC audio callback: %d frames, first peak %d\n", len, peak);
      reported = true;
    }
  }
  pthread_mutex_unlock(&g_spc_audio_mutex);
  return len;
}
#endif

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

static bool ends_with_ci(const char* text, const char* suffix) {
  if (!text || !suffix) return false;
  const size_t text_len = strlen(text);
  const size_t suffix_len = strlen(suffix);
  if (text_len < suffix_len) return false;
  const char* tail = text + text_len - suffix_len;
  for (size_t i = 0; i < suffix_len; i++) {
    if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) return false;
  }
  return true;
}

// SPC files are recognised by extension, with a signature check for files that
// were renamed or have no extension.
static bool is_spc_path(const char* path) {
  if (ends_with_ci(path, ".spc")) return true;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  char signature[27];
  const size_t got = fread(signature, 1, sizeof(signature), f);
  fclose(f);
  static const char kSpcSignature[] = "SNES-SPC700 Sound File Data";
  return got == sizeof(signature) && memcmp(signature, kSpcSignature, sizeof(signature)) == 0;
}

// PSF / miniPSF files, recognised by extension or by the "PSF1" signature.
static bool is_psf_path(const char* path) {
  if (ends_with_ci(path, ".psf") || ends_with_ci(path, ".minipsf")) return true;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  unsigned char signature[4];
  const size_t got = fread(signature, 1, sizeof(signature), f);
  fclose(f);
  return got == 4 && memcmp(signature, "PSF\x01", 4) == 0;
}

static bool read_file_exact(const char* name, std::vector<unsigned char>* out) {
  if (!out) return false;
  out->clear();
  FILE* f = fopen(name, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  const long size = ftell(f);
  if (size <= 0) { fclose(f); return false; }
  rewind(f);
  out->resize((size_t)size);
  const size_t got = fread(out->data(), 1, out->size(), f);
  fclose(f);
  if (got != out->size()) { out->clear(); return false; }
  return true;
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
  } else if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "Not a regular file: %s\n", name);
    close(fd);
    return false;
  }

  int size = st.st_size;
  if (size == 0) {
    fprintf(stderr, "Invalid file size %s\n", name);
    close(fd);
    return false;
  }

  // MXDRVG walks the song data with 32 bit loads and can read a little past the
  // end of the file.  Its own bounds check only detects running off the *low*
  // end (mxdrvg_core.h: L001396), so that over-read is not caught, and the bytes
  // it lands on were whatever the allocator happened to leave behind.  Allocate a
  // zero-filled tail so the read is defined: otherwise a handful of samples of
  // the output differ from run to run.  The browser build already gets this from
  // its calloc'd buffers, which is why it is deterministic.
  const int kTailPad = 64 * 1024;
  u8 *data = (u8 *)calloc(1, (size_t)size + offset + kTailPad);
  if (!data) {
    fprintf(stderr, "Out of memory reading %s\n", name);
    close(fd);
    return false;
  }
  const ssize_t got = read(fd, data + offset, size);
  close(fd);
  if (got < 0) {
    fprintf(stderr, "Cannot read %s.\n", name);
    free(data);
    return false;
  }
  size = (int)got;

  *fdata = data;
  *fsize = size + offset + kTailPad;
  return true;
}

// パスの表記揺れ（"./name" と "name" など）を吸収する。
std::string canonicalPath(const std::string& path) {
  char resolved[PATH_MAX];
  if (realpath(path.c_str(), resolved) != nullptr) return std::string(resolved);
  return path;
}

std::string baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 曲名だけをヘッダから読む（ファイルを再生せずにファイル一覧へ出すため）。
// 文字コードはそのまま（MDX/SPC とも Shift-JIS）返し、表示側で変換する。
std::string readMusicTitle(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return std::string();

  unsigned char head[0x220];
  const size_t got = fread(head, 1, sizeof(head), f);
  fclose(f);

  std::string name = baseName(path);
  auto ends_with = [&name](const char* ext) {
    const size_t n = strlen(ext);
    if (name.size() < n) return false;
    std::string tail = name.substr(name.size() - n);
    for (size_t i = 0; i < n; i++) tail[i] = (char)tolower((unsigned char)tail[i]);
    return tail == ext;
  };

  std::string title;
  if (ends_with(".psf") || ends_with(".minipsf")) {
    // PSF keeps its "[TAG]" block after the compressed program.  Only the tag
    // area is read, so listing a folder stays cheap.
    if (got >= 16) {
      uint32_t reserved = (uint32_t)head[4] | ((uint32_t)head[5] << 8) |
                          ((uint32_t)head[6] << 16) | ((uint32_t)head[7] << 24);
      uint32_t compressed = (uint32_t)head[8] | ((uint32_t)head[9] << 8) |
                            ((uint32_t)head[10] << 16) | ((uint32_t)head[11] << 24);
      const long tag_offset = 16 + (long)reserved + (long)compressed;
      FILE* psf = fopen(path.c_str(), "rb");
      if (psf && fseek(psf, tag_offset, SEEK_SET) == 0) {
        std::string tags;
        char buf[260];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf) - 1, psf)) > 0) {
          buf[n] = 0;
          tags += buf;
          if (tags.size() > 8192) break;
        }
        fclose(psf);
        const size_t tag_start = tags.find("[TAG]");
        if (tag_start != std::string::npos) {
          size_t pos = tag_start + 5;
          while (pos < tags.size()) {
            const size_t eol = tags.find('\n', pos);
            std::string line = tags.substr(pos, eol == std::string::npos
                                                    ? std::string::npos
                                                    : eol - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
              line.pop_back();
            }
            if (line.compare(0, 6, "title=") == 0) {
              title = line.substr(6);
              break;
            }
            if (eol == std::string::npos) break;
            pos = eol + 1;
          }
        }
      } else if (psf) {
        fclose(psf);
      }
    }
  } else if (ends_with(".spc")) {
    // ID666 text tag follows the 0x2E header when bit 0 of 0x23 is set.
    if (got > 0x2E && head[0x23] != 0) {
      size_t len = 0;
      while (len < 32 && 0x2E + len < got && head[0x2E + len] != 0) len++;
      title.assign((const char*)head + 0x2E, len);
    }
  } else {
    // MDX: the title is the plain text after the 10-byte header, ended by CR LF.
    for (size_t i = MAGIC_OFFSET; i < got; i++) {
      if (head[i] == 0 || head[i] == 0x1A) break;
      if (head[i] == 0x0D && i + 1 < got && head[i + 1] == 0x0A) break;
      if (head[i] == 0x0A) break;
      title.push_back((char)head[i]);
    }
  }

  while (!title.empty() &&
         (title.back() == ' ' || title.back() == '\t' || title.back() == '\r' ||
          title.back() == '\n')) {
    title.pop_back();
  }
  return title;
}

struct MusicEntry {
  std::string path;
  bool is_dir;
};

// Directory of a file (or the path itself when it is a directory).
std::string musicDirectory(const char* filepath) {
  struct stat path_stat;
  if (stat(filepath, &path_stat) != 0) return std::string();
  if (S_ISDIR(path_stat.st_mode)) return std::string(filepath);
  std::string path(filepath);
  const size_t last_slash = path.find_last_of('/');
  return (last_slash != std::string::npos) ? path.substr(0, last_slash) : ".";
}

bool hasMusicExtension(const char* name) {
  const size_t len = strlen(name);
  if (len > 4 && strcasecmp(name + len - 4, ".mdx") == 0) return true;
  if (len > 4 && strcasecmp(name + len - 4, ".spc") == 0) return true;
  if (len > 4 && strcasecmp(name + len - 4, ".psf") == 0) return true;
  if (len > 8 && strcasecmp(name + len - 8, ".minipsf") == 0) return true;
  return false;
}

// Files (and subdirectories) in a directory, sorted by path.
std::vector<MusicEntry> listMusicEntries(const std::string& dir_path) {
  std::vector<MusicEntry> entries;
  DIR* dir = opendir(dir_path.c_str());
  if (!dir) return entries;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    const char* name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    const std::string full = dir_path + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      MusicEntry e;
      e.path = full;
      e.is_dir = true;
      entries.push_back(e);
    } else if (S_ISREG(st.st_mode) && hasMusicExtension(name)) {
      MusicEntry e;
      e.path = full;
      e.is_dir = false;
      entries.push_back(e);
    }
  }
  closedir(dir);

  std::sort(entries.begin(), entries.end(),
            [](const MusicEntry& a, const MusicEntry& b) {
              if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;  // dirs first
              return a.path < b.path;
            });
  return entries;
}

// --- Playback history (persisted between runs) -----------------------------
std::string historyPath() {
  const char* home = getenv("HOME");
  if (!home) return std::string();
  return std::string(home) + "/.config/mdx2wav/history.txt";
}

std::vector<std::string> loadHistory() {
  std::vector<std::string> out;
  const std::string path = historyPath();
  if (path.empty()) return out;
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return out;
  char line[4096];
  while (fgets(line, sizeof(line), f) != nullptr) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) out.push_back(s);
    if (out.size() >= 1024) break;
  }
  fclose(f);
  return out;
}

void saveHistory(const std::vector<std::string>& history) {
  const std::string path = historyPath();
  if (path.empty()) return;
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string dir = path.substr(0, slash);
    const size_t parent = dir.find_last_of('/');
    if (parent != std::string::npos && parent > 0) {
      mkdir(dir.substr(0, parent).c_str(), 0755);  // e.g. ~/.config
    }
    mkdir(dir.c_str(), 0755);  // e.g. ~/.config/mdx2wav
  }
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return;
  for (const auto& s : history) fprintf(f, "%s\n", s.c_str());
  fclose(f);
}

// Add a played file to the front of the history (deduplicated, max 1024).
void historyAdd(std::vector<std::string>& history, const std::string& path) {
  const std::string canon = canonicalPath(path);
  for (auto it = history.begin(); it != history.end();) {
    if (canonicalPath(*it) == canon) {
      it = history.erase(it);
    } else {
      ++it;
    }
  }
  history.insert(history.begin(), path);
  if (history.size() > 1024) history.resize(1024);
  saveHistory(history);
}

bool LoadMDX(const char *mdx_name, char *title, int title_len, unsigned int *out_pdx_size = nullptr) {
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

  if (out_pdx_size) {
    *out_pdx_size = pdx_size;
  }

  free(mdx_buf);
  free(pdx_buf);

  return true;
}

#ifdef ENABLE_VISUALIZER
// Run the MDX once without drawing anything to find out which FM/ADPCM
// channels the song actually uses, so the window (and the recorded video) can
// be sized to fit instead of always showing all eight channels.
static void DetectMDXChannels(const char* path, int sample_rate, int filter_mode,
                              int mdx_buf_size, int pdx_buf_size, int* fm_used,
                              int* adpcm_used) {
  *fm_used = 0;
  *adpcm_used = 0;

  YM2151State state;
  OPM_Delegate* original = MXDRVG_GetOPMDelegate();
  X68K::X68PCM8* original_pcm8 = MXDRVG_GetPCM8();
  OPMVisualizer* opm = original ? new OPMVisualizer(original, &state) : nullptr;
  if (opm) MXDRVG_SetOPMDelegate(opm);
  PCM8Visualizer* pcm8 = new PCM8Visualizer(&state);
  MXDRVG_SetPCM8(pcm8);

  // Mirror the main loop's restart sequence: Start, End, Start, then data.
  MXDRVG_Start(sample_rate, filter_mode, mdx_buf_size, pdx_buf_size);
  MXDRVG_End();
  MXDRVG_Start(sample_rate, filter_mode, mdx_buf_size, pdx_buf_size);
  MXDRVG_TotalVolume(256);

  char title[256];
  unsigned int pdx_size = 0;
  long long done = 0;
  if (LoadMDX(path, title, sizeof(title), &pdx_size)) {
    std::vector<short> buf(1024 * 2);
    const long long max_samples = (long long)sample_rate * 300;  // 5 min cap
    while (!MXDRVG_GetTerminated() && done < max_samples) {
      const int n = MXDRVG_GetPCM(buf.data(), 1024);
      if (n <= 0) break;
      done += n;

      YM2151State::Channel channels[8];
      state.getAllChannels(channels);
      for (int ch = 0; ch < 8; ch++) {
        if ((channels[ch].key_on || channels[ch].register_writes > 0) &&
            ch + 1 > *fm_used) {
          *fm_used = ch + 1;
        }
      }
      YM2151State::ADPCMChannel adpcm[8];
      state.getAllADPCMChannels(adpcm);
      for (int ch = 0; ch < 8; ch++) {
        if ((adpcm[ch].key_on || adpcm[ch].initial_address != 0 ||
             adpcm[ch].length > 0) && ch + 1 > *adpcm_used) {
          *adpcm_used = ch + 1;
        }
      }
      if (*fm_used >= 8 && *adpcm_used >= 8) break;  // nothing left to find
    }
  }

  MXDRVG_SetOPMDelegate(original);
  MXDRVG_SetPCM8(original_pcm8);
  delete opm;
  delete pcm8;
}
#endif

void version() {
  printf(
    "mdx2wav version " VERSION "\n"
    "Copyright 2014 @__mtm\n"
    " based on MDXDRVg V1.50a (C) 2000 GORRY.\n"
    "  converted from X68k MXDRV music driver version 2.06+17 Rel.X5-S\n"
    "   (c)1988-92 milk.,K.MAEKAWA, Missy.M, Yatsube\n"
    );
}

#ifdef ENABLE_VISUALIZER
#include <SDL2/SDL.h>
#endif

void help() {
  printf(
    "Usage: mdx2wav [options] <file.mdx|file.spc|file.psf>\n"
    "Convert an MDX, SPC or PSF music file to 16bit stereo raw pcm on stdout.\n"
    " if you need to convert to other format, use ffmpeg like a following.\n"
    "  mdx2wav xxx.mdx | ffmpeg -f s16le -ar 44.1k -ac 2 -i - xxx.wav\n"
    " if you want to listen now, use aplay like a following.\n"
    "  mdx2wav xxx.mdx | aplay -f cd\n"
    "Options:\n"
    "  -d <sec>  : limit song duration. 0 means nolimit. (default:300; for SPC the\n"
    "              ID666 length and for PSF the length tag are used when present)\n"
    "  -e <type> : set ym2151 emulation type, fmgen or mame. (default:fmgen, MDX only)\n"
    "  -f        : enable fadeout.\n"
#ifdef ENABLE_VISUALIZER
    "  -g        : enable visualizer (requires SDL2).\n"
    "  --video <file>      : save as video file (e.g., output.mp4). Uses -d/-l for duration.\n"
    "  --video-fps <n>     : set video framerate (default:60).\n"
    "  --screenshot <file> : save screenshot and exit immediately.\n"
    "  --ym2151-ch <n>     : set number of YM2151 channels to display (1-8, default:8).\n"
    "  --adpcm-ch <n>      : set number of ADPCM channels to display (1-8, default:8).\n"
    "  --waveform-scale <n>: set waveform amplitude scale (0.1-10.0, default:1.0).\n"
    "  --spectrum          : show the spectrum analyzer (off by default; wider window).\n"
    "  --no-auto-size      : keep the default window size instead of fitting it to the\n"
    "                        channels the MDX song actually uses.\n"
#endif
    "  -l <loop> : set loop limit. (default:2, MDX only; SPC loops are controlled by the SPC program)\n"
    "  -m        : measure play time as sec.\n"
    "  --psf-bios <file> : PS1 BIOS image used for .psf playback.  Also read from\n"
    "              $PSF_BIOS, ./psx-bios.bin or ~/.config/mdx2wav/psx-bios.bin.\n"
#ifdef __APPLE__
    "  -p        : play directly to audio device (macOS only).\n"
#endif
    "  -r <rate> : set sampling rate. (default:44100)\n"
    "  -t        : get song title (charset is SHIFT-JIS; SPC ID666 tags are raw bytes).\n"
    "              if you need other charset, try following command:\n"
    "               mdx2wav -t xxx.mdx | iconv -f SHIFT-JIS -t utf-8\n"
    "  -v        : print version.\n"
    "  -V        : verbose, write debug log to stderr.\n"
    "  --volume <n>        : set volume multiplier (0.0-2.0, default:1.0).\n"
    "  --swap-channels     : swap left/right audio channels.\n"
    );
}



int main(int argc, char **argv) {
  CommandLineOptions options;
  CommandLineParser parser(argc, argv);
  std::string parse_error;
  if (!parser.Parse(&options, &parse_error)) {
    if (!parse_error.empty()) {
      fprintf(stderr, "%s\n", parse_error.c_str());
    }
    help();
    return -1;
  }

  if (options.show_help) {
    help();
    return 0;
  }

  if (options.show_version) {
    version();
    return 0;
  }

  verbose = options.verbose_logging;

  int MDX_BUF_SIZE = options.mdx_buf_size;
  int PDX_BUF_SIZE = options.pdx_buf_size;
  int SAMPLE_RATE = options.sample_rate;
  int filter_mode = options.filter_mode;

  bool measure_play_time = options.measure_play_time;
  bool get_title = options.get_title;
  bool play_audio = options.play_audio;
  bool enable_visualizer = options.enable_visualizer;
  bool swap_channels = options.swap_channels;
  float volume = options.volume;
  float max_song_duration = options.max_song_duration;
  int loop = options.loop;
  int fadeout = options.fadeout;
  char ym2151_type[8];
  strncpy(ym2151_type, options.ym2151_type, sizeof(ym2151_type));
  ym2151_type[sizeof(ym2151_type) - 1] = 0;

  const char* video_filename = nullptr;
#ifdef ENABLE_VISUALIZER
  if (!options.video_filename.empty()) {
    video_filename = options.video_filename.c_str();
  }
  float ym2151_waveform_scale = options.ym2151_waveform_scale;
  float adpcm_waveform_scale = options.adpcm_waveform_scale;
  const char* screenshot_filename = options.screenshot_filename.empty() ? nullptr : options.screenshot_filename.c_str();
  const char* spectrum_debug_filename = options.spectrum_debug_filename.empty() ? nullptr : options.spectrum_debug_filename.c_str();
  int video_fps = options.video_fps;
  int ym2151_channels = options.ym2151_channels;
  int adpcm_channels = options.adpcm_channels;
#endif

  int AUDIO_BUF_SAMPLES = SAMPLE_RATE / 100; // 10ms

  // Check for conflicting options
  if (play_audio && video_filename) {
    fprintf(stderr, "Error: -p and --video options cannot be used together.\n");
    return -1;
  }

  const char *mdx_name = options.mdx_input.empty() ? nullptr : options.mdx_input.c_str();
  if (mdx_name == 0 || *mdx_name == 0) {
    help();
    return 0;
  }

  // The selected path has to outlive the local file list.
  std::string selected_file;

  // ディレクトリが指定された場合の処理（ビジュアライザー無効時）
  if (!enable_visualizer) {
    struct stat path_stat;
    if (stat(mdx_name, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      std::vector<MusicEntry> entries = listMusicEntries(mdx_name);
      bool found = false;
      for (const auto& e : entries) {
        if (e.is_dir) continue;
        selected_file = e.path;
        mdx_name = selected_file.c_str();
        found = true;
        break;
      }
      if (found) {
        fprintf(stderr, "Directory specified. Playing first file: %s\n", mdx_name);
      } else {
        fprintf(stderr, "No MDX/SPC/PSF files found in directory: %s\n", mdx_name);
        return -1;
      }
    }
  }

  bool is_spc = is_spc_path(mdx_name);
  bool is_psf = !is_spc && is_psf_path(mdx_name);

  // Recently played files, persisted between runs (up to 1024).
  std::vector<std::string> history = loadHistory();

  if (0 == strcmp(ym2151_type, "fmgen")) {
  } else if (0 == strcmp(ym2151_type, "mame")) {
    MXDRVG_SetEmulationType(MXDRVG_YM2151TYPE_MAME);
  } else {
    fprintf(stderr, "Invalid ym2151 emulation type: %s.\n", ym2151_type);
    return -1;
  }

#ifdef ENABLE_VISUALIZER
  Visualizer* visualizer = nullptr;
  YM2151State* ym_state = nullptr;
  OPM_Delegate* opm_original = nullptr;
  OPMVisualizer* opm_wrapper = nullptr;
  PCM8Visualizer* pcm8_wrapper = nullptr;

  // ファイルリストを準備（ビジュアライザー用）
  std::vector<std::string> file_list;
  std::vector<std::string> file_titles;
  std::vector<uint8_t> file_dirs;
  std::vector<const char*> file_ptrs;
  std::vector<const char*> title_ptrs;
  std::string music_dir;
  int current_file_index = 0;

  auto rebuildFileList = [&](const std::string& dir) {
    music_dir = dir;
    file_list.clear();
    file_titles.clear();
    file_dirs.clear();
    file_ptrs.clear();
    title_ptrs.clear();
    std::vector<MusicEntry> entries = listMusicEntries(dir);
    for (const auto& e : entries) {
      file_list.push_back(e.path);
      file_titles.push_back(e.is_dir ? std::string() : readMusicTitle(e.path));
      file_dirs.push_back(e.is_dir ? 1 : 0);
    }
    for (const auto& f : file_list) file_ptrs.push_back(f.c_str());
    for (const auto& t : file_titles) title_ptrs.push_back(t.c_str());
  };

  if (enable_visualizer) {
    // Absolute path so the filer's parent-directory navigation is stable.
    rebuildFileList(canonicalPath(musicDirectory(mdx_name)));

    // ディレクトリが指定された場合、最初のファイルを再生
    struct stat path_stat;
    if (stat(mdx_name, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      bool found = false;
      for (size_t i = 0; i < file_list.size(); i++) {
        if (!file_dirs[i]) {
          mdx_name = file_list[i].c_str();
          current_file_index = (int)i;
          found = true;
          break;
        }
      }
      if (!found) {
        fprintf(stderr, "No MDX/SPC/PSF files found in directory: %s\n", mdx_name);
        return -1;
      }
      fprintf(stderr, "Directory specified. Playing first file: %s\n", mdx_name);
    } else if (!file_list.empty()) {
      // 現在のファイルのインデックスを見つける。  カレントディレクトリから
      // 再生した場合は "./name" と "name" のように表記が揺れるため、実パス
      // で比較する（見つからなければファイル名だけで比較する）。
      const std::string current_file = canonicalPath(mdx_name);
      bool found = false;
      for (size_t i = 0; i < file_list.size(); i++) {
        if (file_dirs[i]) continue;
        if (canonicalPath(file_list[i]) == current_file) {
          current_file_index = (int)i;
          found = true;
          break;
        }
      }
      if (!found) {
        const std::string base = baseName(mdx_name);
        for (size_t i = 0; i < file_list.size(); i++) {
          if (file_dirs[i]) continue;
          if (baseName(file_list[i]) == base) {
            current_file_index = (int)i;
            break;
          }
        }
      }
    }
    is_spc = is_spc_path(mdx_name);
    is_psf = !is_spc && is_psf_path(mdx_name);

    // YM2151状態管理オブジェクト作成
    ym_state = new YM2151State();
    ym_state->setSPCMode(is_spc);

    // ウィンドウの高さを計算（表示チャンネル数に応じて調整）
    // タイトル: 50px, Timer情報: 15px, 区切り線: 10px = 75px
    // YM2151チャンネル: 70px/ch
    // ADPCMチャンネル: 45px/ch
    // 下部余白: 20px
    // The SPC layout uses the same tall window so its status panel fits, but
    // it has no ADPCM section.
    if (is_spc) {
      ym2151_channels = 8;
      adpcm_channels = 0;
    } else if (!is_psf && !options.no_auto_size && !get_title &&
               !measure_play_time) {
      // Fit the window to the channels the song really uses.
      int fm_used = 0, adpcm_used = 0;
      DetectMDXChannels(mdx_name, SAMPLE_RATE, filter_mode, MDX_BUF_SIZE,
                        PDX_BUF_SIZE, &fm_used, &adpcm_used);
      if (fm_used > 0 && fm_used < ym2151_channels) ym2151_channels = fm_used;
      if (adpcm_used < adpcm_channels) adpcm_channels = adpcm_used;
      if (verbose) {
        fprintf(stderr, "auto size: FM %d ch, ADPCM %d ch\n",
                ym2151_channels, adpcm_channels);
      }
    }
    int window_height = is_psf
        ? 844
        : (is_spc ? 720
                  : 75 + (ym2151_channels * 70) + (adpcm_channels * 45) + 20);
    // The spectrum strip is opt-in (--spectrum); without it the window is
    // narrower.  PSF never shows a spectrum.
    const bool want_spectrum = options.enable_spectrum && !is_psf;
    const int window_width = want_spectrum ? 1400 : 1080;

    // 動画モードの場合は高さを偶数に調整（H.264要件）
    if (video_filename && (window_height % 2) != 0) {
      window_height++;
    }

    // ビジュアライザー初期化
    visualizer = new Visualizer();
    visualizer->setSpectrumEnabled(want_spectrum);
    visualizer->setSampleRate(SAMPLE_RATE);
    bool init_success = false;

    if (video_filename) {
      // 動画録画モード
      init_success = visualizer->initVideoMode(video_filename, window_width, window_height, video_fps, SAMPLE_RATE);
      if (!init_success) {
        fprintf(stderr, "Failed to initialize visualizer in video mode\n");
        delete visualizer;
        delete ym_state;
        return -1;
      }
      fprintf(stderr, "Video recording mode: %s (%dfps)\n", video_filename, video_fps);
    } else {
      // 通常の表示モード
      init_success = visualizer->init(
          is_spc ? "SPC Visualizer" : (is_psf ? "PSF Visualizer" : "MDX Visualizer"),
          window_width, window_height);
      if (!init_success) {
        fprintf(stderr, "Failed to initialize visualizer\n");
        delete visualizer;
        delete ym_state;
        return -1;
      }
    }

    visualizer->setState(ym_state);
    visualizer->setSPCMode(is_spc);
    visualizer->setPSFMode(is_psf);

    // 表示チャンネル数を設定
    visualizer->setDisplayChannels(is_spc ? 8 : ym2151_channels,
                                  is_spc ? 0 : adpcm_channels);

    // 波形スケールを設定
    visualizer->setWaveformScale(ym2151_waveform_scale, adpcm_waveform_scale);

    // スペクトラムデバッグモードを設定（指定時はスペクトラムも有効化）
    if (spectrum_debug_filename) {
      visualizer->setSpectrumEnabled(true);
      visualizer->setSpectrumDebug(spectrum_debug_filename);
    }

    // ファイルリストを設定。  ファイル名だけだと選びにくいので、ヘッダから
    // 読んだ曲名も一緒に渡す（フィルタモードで表示する）。
    if (!file_list.empty()) {
      visualizer->setFileList(file_ptrs.data(), title_ptrs.data(),
                              file_dirs.data(), (int)file_ptrs.size(),
                              current_file_index);
    }
    visualizer->setDirectory(music_dir.c_str());
    {
      std::vector<const char*> history_ptrs;
      for (const auto& h : history) history_ptrs.push_back(h.c_str());
      visualizer->setHistory(history_ptrs.data(), (int)history_ptrs.size());
    }

    // スクリーンショット専用モードの設定
    if (screenshot_filename) {
      visualizer->setScreenshotMode(screenshot_filename);
    }
  }
#endif

#ifdef ENABLE_VISUALIZER
  // OPM_Delegateをラップしてレジスタ監視を有効化
  if (enable_visualizer && ym_state) {
    opm_original = MXDRVG_GetOPMDelegate();
    if (opm_original) {
      opm_wrapper = new OPMVisualizer(opm_original, ym_state);
      MXDRVG_SetOPMDelegate(opm_wrapper);

      // Visualizerとの連携を設定 (ミュート機能とキーボード演奏のため)
      if (visualizer) {
        opm_wrapper->setVisualizer(visualizer);
        visualizer->setOPMWrapper(opm_wrapper);
      }

      if (verbose) {
        fprintf(stderr, "YM2151 register monitoring enabled\n");
      }
    }

    // PCM8をラップしてADPCM再生を監視
    pcm8_wrapper = new PCM8Visualizer(ym_state);
    MXDRVG_SetPCM8(pcm8_wrapper);

    if (verbose) {
      fprintf(stderr, "PCM8 ADPCM monitoring enabled\n");
    }
  }
#endif

  SPCPlayer* spc_player = nullptr;
  PSFPlayer* psf_player = nullptr;
  bool mdx_started = false;

  if (!is_spc && !is_psf) {
    MXDRVG_Start(SAMPLE_RATE, filter_mode, MDX_BUF_SIZE, PDX_BUF_SIZE);
    MXDRVG_TotalVolume(256);
    mdx_started = true;
  }

  // visualizerモードでは連続再生を有効化
  g_continuous_playback = enable_visualizer;

  short *audio_buf = new short [AUDIO_BUF_SAMPLES * 2];

  // Holds the path of a file chosen from the history view.  It must live
  // before the reload label: mdx_name points into it, and a goto back to the
  // label must not leave its scope.
  std::string requested_path_storage;

#ifdef __APPLE__
  AudioContext audioCtx = {0};
  bool audio_queue_initialized = false;
#endif

reload_file:
  // A directory may contain several formats; decide again for every file.
  is_spc = is_spc_path(mdx_name);
  is_psf = !is_spc && is_psf_path(mdx_name);

#ifdef ENABLE_VISUALIZER
  if (visualizer) {
    visualizer->setSPCMode(is_spc);
    visualizer->setPSFMode(is_psf);
    visualizer->setDisplayChannels(is_spc ? 8 : ym2151_channels,
                                  is_spc ? 0 : adpcm_channels);
  }
  if (ym_state) {
    ym_state->reset();
    ym_state->setSPCMode(is_spc);
  }
#endif

  // ファイル切り替え時はエンジンをリセット
  if (mdx_started) {
    MXDRVG_End();
    mdx_started = false;
  }
  if (!is_spc && !is_psf) {
    MXDRVG_Start(SAMPLE_RATE, filter_mode, MDX_BUF_SIZE, PDX_BUF_SIZE);
    MXDRVG_TotalVolume(256);
    mdx_started = true;
  }

  char title[256];
  title[0] = 0;
  unsigned int pdx_size = 0;

  if (is_spc) {
    std::vector<unsigned char> spc_data;
    if (!read_file_exact(mdx_name, &spc_data)) {
      fprintf(stderr, "Cannot open/read %s.\n", mdx_name);
      return -1;
    }
    if (!spc_player) {
      spc_player = new SPCPlayer();
#ifdef __APPLE__
      g_spc_audio_player = spc_player;
#endif
    }
#ifdef __APPLE__
    pthread_mutex_lock(&g_spc_audio_mutex);
#endif
    std::string spc_error;
    bool spc_loaded = spc_player->Load(spc_data.data(), spc_data.size(), &spc_error);
    if (spc_loaded) {
      spc_player->SetSampleRate(SAMPLE_RATE);
      spc_player->Start(loop, max_song_duration, fadeout ? 3.0 : 0.0);
    }
#ifdef __APPLE__
    pthread_mutex_unlock(&g_spc_audio_mutex);
#endif
    if (!spc_loaded) {
      fprintf(stderr, "Cannot load SPC %s: %s\n", mdx_name, spc_error.c_str());
      return -1;
    }
    strncpy(title, spc_player->Title(), sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
    if (title[0] == 0) {
      const char* base = strrchr(mdx_name, '/');
      strncpy(title, base ? base + 1 : mdx_name, sizeof(title) - 1);
      title[sizeof(title) - 1] = 0;
    }
#ifdef __APPLE__
    setAudioPCMFillFunc(SpcAudioFill);
#endif
  } else if (is_psf) {
    if (!psf_player) {
      psf_player = new PSFPlayer();
#ifdef __APPLE__
      g_psf_audio_player = psf_player;
#endif
    }
#ifdef __APPLE__
    pthread_mutex_lock(&g_psf_audio_mutex);
#endif
    std::string psf_error;
    const bool psf_loaded = psf_player->Load(
        mdx_name, options.psf_bios_filename.c_str(), &psf_error);
    if (psf_loaded) {
      psf_player->SetSampleRate(SAMPLE_RATE);
      psf_player->Start(loop, max_song_duration, fadeout ? 3.0 : 0.0);
    }
#ifdef __APPLE__
    pthread_mutex_unlock(&g_psf_audio_mutex);
#endif
    if (!psf_loaded) {
      fprintf(stderr, "Cannot load PSF %s: %s\n", mdx_name, psf_error.c_str());
      return -1;
    }
    strncpy(title, psf_player->Title(), sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
    if (title[0] == 0) {
      const char* base = strrchr(mdx_name, '/');
      strncpy(title, base ? base + 1 : mdx_name, sizeof(title) - 1);
      title[sizeof(title) - 1] = 0;
    }
#ifdef __APPLE__
    setAudioPCMFillFunc(PsfAudioFill);
#endif
  } else {
#ifdef __APPLE__
    setAudioPCMFillFunc(nullptr);
#endif
    if (!LoadMDX(mdx_name, title, sizeof(title), &pdx_size)) {
      return -1;
    }
  }

#ifdef ENABLE_VISUALIZER
  // ビジュアライザーにタイトルとファイル名を設定
  if (visualizer && ym_state) {
    visualizer->setSongTitle(title);
    visualizer->setFilename(mdx_name);
    visualizer->resetElapsedTime();

    // ファイルリストのインデックスを更新（ポインタは初回設定時のものを
    // そのまま使う。ここで新しいベクタを作ると即座に破棄されてしまう）
    if (!file_list.empty()) {
      visualizer->setCurrentFileIndex(current_file_index);
    }

    if (!is_spc && !is_psf) {
      // ADPCM全体バッファ情報を取得して設定
      void* adpcm_buffer = MXDRVG_GetADPCMBuffer();
      ym_state->setADPCMBufferInfo(adpcm_buffer, pdx_size);

      if (verbose) {
        fprintf(stderr, "ADPCM buffer: %p, size: %u bytes (PDX data size)\n", adpcm_buffer, pdx_size);
      }
    }
  }
#endif

  // Remember this song for the history view.
  historyAdd(history, mdx_name);
#ifdef ENABLE_VISUALIZER
  if (visualizer) {
    std::vector<const char*> history_ptrs;
    for (const auto& h : history) history_ptrs.push_back(h.c_str());
    visualizer->setHistory(history_ptrs.data(), (int)history_ptrs.size());
  }
#endif

  if (get_title) {
    printf("%s\n", title);
    return 0;
  }

  float song_duration = 0.0f;
  if (is_spc) {
    song_duration = (float)spc_player->Duration();
  } else if (is_psf) {
    song_duration = (float)psf_player->Duration();
  } else {
    song_duration = MXDRVG_MeasurePlayTime(loop, fadeout) / 1000.0f;
    // Warning: MXDRVG_MeasurePlayTime calls MXDRVG_End internaly,
    //          thus we need to call MXDRVG_PlayAt due to reset playing status.
    MXDRVG_PlayAt(0, loop, fadeout);
  }

  if (measure_play_time) {
    printf("%d\n", (int) ceilf(song_duration));
    return 0;
  }

  if (verbose) {
    fprintf(stderr, "loop:%d fadeout:%d song_duration:%f\n", loop, fadeout, song_duration);
  }

  {
    if (max_song_duration < song_duration) {
      song_duration = max_song_duration;
    }
  }

  // In direct playback mode the audio callback advances the song clock.  The
  // loop below only drives the UI, so its 10 ms-per-iteration duration check
  // would cut the song short; let the callback decide when it ends.
  if (play_audio) {
    song_duration = 0.0f;
  }

#ifdef __APPLE__
  if (play_audio && !audio_queue_initialized) {
    // Start the queue only after the song is loaded (and, for SPC, after
    // SPCPlayer::Start()), so the callback never sees a half-loaded engine.
    if (!initAudioQueue(&audioCtx, SAMPLE_RATE, AUDIO_BUF_SAMPLES)) {
      fprintf(stderr, "Failed to initialize audio playback.\n");
      if (mdx_started) MXDRVG_End();
      delete spc_player;
      delete []audio_buf;
      return -1;
    }
    audio_queue_initialized = true;
    if (verbose) {
      fprintf(stderr, "Playing audio... (Press Ctrl+C to stop)\n");
    }
  }
#endif

  // Main loop
  double total_audio_time = 0.0;  // 動画モード用: 累積オーディオ時間
  int video_frames_rendered = 0;   // 動画モード用: レンダリング済みフレーム数

  for (int i = 0; song_duration == 0.0f || 1.0f * i * AUDIO_BUF_SAMPLES / SAMPLE_RATE < song_duration; i++) {
#ifdef ENABLE_VISUALIZER
    // 動画モード: 累積オーディオ時間で正確に制御
    if (song_duration > 0.0f && total_audio_time >= song_duration) {
      break;
    }

    // fmgen / S-DSP から表示用の状態を更新
    if (enable_visualizer && ym_state) {
      if (is_spc) {
        if (spc_player) {
#ifdef __APPLE__
          pthread_mutex_lock(&g_spc_audio_mutex);
#endif
          UpdateSPCVisualizerState(*spc_player, ym_state);
#ifdef __APPLE__
          pthread_mutex_unlock(&g_spc_audio_mutex);
#endif
        }
      } else if (is_psf) {
        if (psf_player && visualizer) {
          PSFPlayer::VoiceState pv[PSFPlayer::kVoices];
#ifdef __APPLE__
          pthread_mutex_lock(&g_psf_audio_mutex);
#endif
          psf_player->GetVoiceStates(pv);
          PSFPlayer::GlobalState gs;
          psf_player->GetGlobalState(&gs);
#ifdef __APPLE__
          pthread_mutex_unlock(&g_psf_audio_mutex);
#endif
          Visualizer::PSFVoice out[PSFPlayer::kVoices];
          for (int vi = 0; vi < PSFPlayer::kVoices; vi++) {
            out[vi].active = pv[vi].active;
            out[vi].note_on = pv[vi].note_on;
            out[vi].pitch = pv[vi].pitch;
            out[vi].note = pv[vi].note;
            out[vi].volume_left = pv[vi].volume_left;
            out[vi].volume_right = pv[vi].volume_right;
            out[vi].envelope = pv[vi].envelope;
            out[vi].adsr1 = pv[vi].adsr1;
            out[vi].adsr2 = pv[vi].adsr2;
            out[vi].phase = pv[vi].phase;
            out[vi].start_address = pv[vi].start_address;
            out[vi].loop_address = pv[vi].loop_address;
            out[vi].current_address = pv[vi].current_address;
            for (int wi = 0; wi < PSFPlayer::kWaveformSamples; wi++) {
              out[vi].waveform[wi] = pv[vi].waveform[wi];
            }
            out[vi].waveform_samples = pv[vi].waveform_samples;
            out[vi].sample_start = pv[vi].sample_start;
            out[vi].sample_end = pv[vi].sample_end_address;
          }
          visualizer->updatePSFState(out, PSFPlayer::kVoices,
                                     gs.master_volume_left, gs.master_volume_right,
                                     gs.reverb_enabled != 0);
        }
      } else if (opm_original) {
        void* opm_ptr = opm_original->GetOPMPointer();
        if (opm_ptr) {
          ym_state->updateFromFmgen(opm_ptr);
          // visualizerにOPMポインタを設定（Timerトグル用）
          if (visualizer) {
            visualizer->setOPMPointer(opm_ptr);
          }
        }
      }
    }

    // Update visualizer (must be on main thread for macOS)
    if (enable_visualizer && visualizer && !video_filename) {
      // 通常モード: 既存の処理
      // MXDRVGの一時停止状態を適用
      static bool last_pause_state = false;
      static bool spc_paused = false;
      bool current_pause_state = visualizer->isMXDRVGPaused();
      if (current_pause_state != last_pause_state) {
        if (is_spc || is_psf) {
          spc_paused = current_pause_state;
        } else if (current_pause_state) {
          MXDRVG_Pause();  // レジスタ書き込みを停止
        } else {
          MXDRVG_Cont();   // レジスタ書き込みを再開
        }
        last_pause_state = current_pause_state;
      }
      visualizer->update();

        // 曲の再スタート要求をチェック
        if (visualizer->hasRestartRequest()) {
          visualizer->clearRestartRequest();

          // 現在の曲を頭から再生
          fprintf(stderr, "\nRestarting song: %s\n", mdx_name);
          goto reload_file;
        }

        // 履歴から曲が選ばれたら、その曲のフォルダに一覧を作り直して再生する
        if (visualizer->hasPathChangeRequest()) {
          const std::string path = visualizer->getRequestedPath();
          visualizer->clearPathChangeRequest();
          struct stat st;
          if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            rebuildFileList(canonicalPath(musicDirectory(path.c_str())));
            int idx = 0;
            const std::string canon = canonicalPath(path);
            for (size_t i = 0; i < file_list.size(); i++) {
              if (!file_dirs[i] && canonicalPath(file_list[i]) == canon) {
                idx = (int)i;
                break;
              }
            }
            visualizer->setFileList(file_ptrs.data(), title_ptrs.data(),
                                    file_dirs.data(), (int)file_ptrs.size(), idx);
            visualizer->setDirectory(music_dir.c_str());
            std::vector<const char*> hp;
            for (const auto& h : history) hp.push_back(h.c_str());
            visualizer->setHistory(hp.data(), (int)hp.size());
            current_file_index = idx;
            requested_path_storage = path;
            mdx_name = requested_path_storage.c_str();
            fprintf(stderr, "\nSwitching to: %s\n", mdx_name);
            goto reload_file;
          }
          fprintf(stderr, "History entry not found: %s\n", path.c_str());
        }

        // フィルタでディレクトリ移動が要求されたら一覧を作り直す
        if (visualizer->hasDirectoryChangeRequest()) {
          const std::string dir = visualizer->getRequestedDirectory();
          visualizer->clearDirectoryChangeRequest();
          rebuildFileList(dir);
          visualizer->setFileList(file_ptrs.data(), title_ptrs.data(),
                                  file_dirs.data(), (int)file_ptrs.size(), 0);
          visualizer->setDirectory(music_dir.c_str());
          visualizer->applyPendingSelection();
          current_file_index = 0;
        }

        // ファイル切り替え要求をチェック
        if (visualizer->hasFileChangeRequest()) {
          int new_index = visualizer->getRequestedFileIndex();
          if (new_index >= 0 && new_index < (int)file_list.size()) {
            mdx_name = file_list[new_index].c_str();
            current_file_index = new_index;
            visualizer->clearFileChangeRequest();

            // 再生を停止して新しいファイルをロード
            fprintf(stderr, "\nSwitching to: %s\n", mdx_name);
            // ファイル切り替え時は完全にリセット
            goto reload_file;
          }
        }

        if (!visualizer->isRunning()) {
          break;
        }
        if ((is_spc || is_psf) && spc_paused) {
          usleep(16 * 1000);
          continue;
        }
    }
#endif

    // ビジュアライザー使用時は曲の終了を無視（ファイル切り替えや手動終了まで継続）
    // ただし動画録画モードの場合は曲終了を検知して停止
    if (is_spc) {
      if (!play_audio && spc_player && spc_player->IsFinished()) {
        break;
      }
    } else if (is_psf) {
      if (!play_audio && psf_player && psf_player->IsFinished()) {
        break;
      }
    } else {
#ifdef ENABLE_VISUALIZER
      if (!enable_visualizer || video_filename)
#endif
      {
        if (MXDRVG_GetTerminated()) {
          break;
        }
      }
    }

#ifdef __APPLE__
    // Audio playback mode - just wait
    if (play_audio) {
#ifdef ENABLE_VISUALIZER
      // ビジュアライザー使用時は共有バッファからオーディオデータを取得
      if (enable_visualizer && visualizer) {
        pthread_mutex_lock(&audioCtx.buffer_mutex);
        if (audioCtx.shared_buffer && audioCtx.shared_buffer_len > 0) {
          visualizer->updateWaveform(audioCtx.shared_buffer, audioCtx.shared_buffer_len);
        }
        pthread_mutex_unlock(&audioCtx.buffer_mutex);
      }

      // With -g -p the visualizer keeps running after the song ends, so the
      // user can restart or pick another file; only plain -p playback (and
      // the video path, which does not reach here) stops on termination.
      if (!enable_visualizer)
#endif
      {
        if (audioCtx.terminated) {
          fprintf(stderr, "Audio playback terminated.\n");
          break;
        }
      }
      usleep(16 * 1000); // ~60fps for visualizer
      continue;
    }
#endif

    // File output mode or video mode
    // 動画モード: song_durationを超えないように必要なサンプル数を計算
    int samples_to_get = AUDIO_BUF_SAMPLES;
#ifdef ENABLE_VISUALIZER
    if (video_filename && song_duration > 0.0f) {
      double remaining_time = song_duration - total_audio_time;
      if (remaining_time <= 0.0) {
        break;  // 既に指定時間に達している
      }
      int remaining_samples = (int)(remaining_time * SAMPLE_RATE);
      if (remaining_samples < samples_to_get) {
        samples_to_get = remaining_samples;
      }
    }
#endif

    int len;
    if (is_spc) {
      if (!spc_player || spc_player->IsFinished()) break;
      len = spc_player->Generate((int16_t*)audio_buf, samples_to_get);
    } else if (is_psf) {
      if (!psf_player || psf_player->IsFinished()) break;
      len = psf_player->Generate((int16_t*)audio_buf, samples_to_get);
    } else {
      len = MXDRVG_GetPCM(audio_buf, samples_to_get);
    }
    if (len <= 0) {
      break;
    }

    // Swap left/right channels if requested
    if (swap_channels) {
      for (int i = 0; i < len; i++) {
        short temp = audio_buf[i * 2];
        audio_buf[i * 2] = audio_buf[i * 2 + 1];
        audio_buf[i * 2 + 1] = temp;
      }
    }

    // Apply volume adjustment
    if (volume != 1.0f) {
      for (int i = 0; i < len * 2; i++) {
        int sample = (int)(audio_buf[i] * volume);
        // Clamp to prevent overflow
        if (sample > 32767) sample = 32767;
        else if (sample < -32768) sample = -32768;
        audio_buf[i] = (short)sample;
      }
    }

#ifdef ENABLE_VISUALIZER
    // 動画録画モード: オーディオサンプルを先に更新してからフレームをレンダリング
    if (enable_visualizer && visualizer) {
      visualizer->updateWaveform(audio_buf, len);

      if (video_filename) {
        // 動画モード: オーディオ時間を累積
        total_audio_time += (double)len / SAMPLE_RATE;

        // 次のビデオフレームをレンダリングする時刻を計算
        double next_frame_time = (video_frames_rendered + 1) / (double)video_fps;

        // オーディオ時間が次のフレーム時刻を超えた場合、フレームをレンダリング
        while (total_audio_time >= next_frame_time) {
          if (!visualizer->renderVideoFrame()) {
            break;  // エンコードエラー発生
          }
          video_frames_rendered++;
          next_frame_time = (video_frames_rendered + 1) / (double)video_fps;
        }
      }
    }
#endif

    // 動画録画モード以外の場合のみファイル出力
    if (!video_filename) {
      fwrite(audio_buf, len, 4, stdout);
    }
  }

#ifdef __APPLE__
  if (play_audio && audio_queue_initialized) {
    cleanupAudioQueue(&audioCtx);
  }
  setAudioPCMFillFunc(nullptr);
  pthread_mutex_lock(&g_spc_audio_mutex);
  g_spc_audio_player = nullptr;
  pthread_mutex_unlock(&g_spc_audio_mutex);
#endif

  delete []audio_buf;

  if (mdx_started) {
    MXDRVG_End();
  }
  delete spc_player;
  spc_player = nullptr;

#ifdef ENABLE_VISUALIZER
  if (enable_visualizer) {
    if (visualizer) {
      visualizer->shutdown();
      delete visualizer;
    }
    if (opm_wrapper) {
      delete opm_wrapper;
    }
    if (ym_state) {
      delete ym_state;
    }
  }
#endif

  if (verbose) {
    fprintf(stderr, "completed.\n");
  }
  return 0;
}
