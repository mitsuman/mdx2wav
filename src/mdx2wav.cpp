#include <ctype.h>
#include <fcntl.h>
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

#ifdef __APPLE__
#include "platform_mac.h"
#endif

#ifdef ENABLE_VISUALIZER
#include "visualizer/visualizer.h"
#include "visualizer/ym2151_state.h"
#include "../gamdx/mxdrvg/opm_visualizer.h"
#include "../gamdx/mxdrvg/pcm8_visualizer.h"
#include <pthread.h>
#endif

#define VERSION "1.0"

bool verbose = false;
bool g_continuous_playback = false;

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

// ディレクトリ内のMDX/PDXファイルを列挙
std::vector<std::string> listMDXFiles(const char* filepath) {
  std::vector<std::string> files;
  
  // まず、パスがディレクトリかファイルかを確認
  struct stat path_stat;
  if (stat(filepath, &path_stat) != 0) {
    return files;
  }
  
  std::string dir_path;
  bool is_directory = S_ISDIR(path_stat.st_mode);
  
  if (is_directory) {
    // ディレクトリが直接指定された場合
    dir_path = filepath;
  } else {
    // ファイルが指定された場合、そのディレクトリを取得
    std::string path(filepath);
    size_t last_slash = path.find_last_of('/');
    dir_path = (last_slash != std::string::npos) ? path.substr(0, last_slash) : ".";
  }
  
  DIR* dir = opendir(dir_path.c_str());
  if (!dir) {
    return files;
  }
  
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type == DT_REG) {
      const char* name = entry->d_name;
      size_t len = strlen(name);
      
      // .mdx または .MDX で終わるファイルを追加
      if (len > 4 && 
          ((name[len-4] == '.' && name[len-3] == 'm' && name[len-2] == 'd' && name[len-1] == 'x') ||
           (name[len-4] == '.' && name[len-3] == 'M' && name[len-2] == 'D' && name[len-1] == 'X'))) {
        std::string fullpath = dir_path + "/" + name;
        files.push_back(fullpath);
      }
    }
  }
  
  closedir(dir);
  
  // ソート
  std::sort(files.begin(), files.end());
  
  return files;
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

  delete []mdx_buf;
  delete []pdx_buf;

  return true;
}

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
    "Usage: mdx2wav [options] <file>\n"
    "Convert mdx file to 16bit stereo raw pcm and write it to stdout.\n"
    " if you need to convert to other format, use ffmpeg like a following.\n"
    "  mdx2wav xxx.mdx | ffmpeg -f s16le -ar 44.1k -ac 2 -i - xxx.wav\n"
    " if you want to listen now, use aplay like a following.\n"
    "  mdx2wav xxx.mdx | aplay -f cd\n"
    "Options:\n"
    "  -d <sec>  : limit song duration. 0 means nolimit. (default:300)\n"
    "  -e <type> : set ym2151 emulation type, fmgen or mame. (default:fmgen)\n"
    "  -f        : enable fadeout.\n"
#ifdef ENABLE_VISUALIZER
    "  -g        : enable visualizer (requires SDL2).\n"
    "  --video <file>      : save as video file (e.g., output.mp4). Uses -d/-l for duration.\n"
    "  --video-fps <n>     : set video framerate (default:60).\n"
    "  --screenshot <file> : save screenshot and exit immediately.\n"
    "  --ym2151-ch <n>     : set number of YM2151 channels to display (1-8, default:8).\n"
    "  --adpcm-ch <n>      : set number of ADPCM channels to display (1-8, default:8).\n"
    "  --waveform-scale <n>: set waveform amplitude scale (0.1-10.0, default:1.0).\n"
#endif
    "  -l <loop> : set loop limit. (default:2)\n"
    "  -m        : measure play time as sec.\n"
#ifdef __APPLE__
    "  -p        : play directly to audio device (macOS only).\n"
#endif
    "  -r <rate> : set sampling rate. (default:44100)\n"
    "  -t        : get song title (charset is SHIFT-JIS).\n"
    "              if you need other charset, try following command:\n"
    "               mdx2wav -t xxx.mdx | iconv -f SHIFT-JIS -t utf-8\n"
    "  -v        : print version.\n"
    "  -V        : verbose, write debug log to stderr.\n"
    "  --volume <n>        : set volume multiplier (0.0-2.0, default:1.0).\n"
    "  --swap-channels     : swap left/right audio channels.\n"
    );
}



int main(int argc, char **argv) {
  // Check for -h or --help anywhere in arguments first
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      help();
      return 0;
    }
  }
  
  int MDX_BUF_SIZE = 256 * 1024;
  int PDX_BUF_SIZE = 1024 * 1024;
  int SAMPLE_RATE = 44100;
  int filter_mode = 0;

  bool measure_play_time = false;
  bool get_title = false;
  bool play_audio = false;
  bool enable_visualizer = false;
  bool swap_channels = false;
  float volume = 1.0f;
  float ym2151_waveform_scale = 1.0f;
  float adpcm_waveform_scale = 1.0f;
  const char* screenshot_filename = nullptr;
  const char* spectrum_debug_filename = nullptr;
  const char* video_filename = nullptr;
  int video_fps = 60;
  int ym2151_channels = 8;
  int adpcm_channels = 8;
  float max_song_duration = 300.0f;
  int loop = 2;
  int fadeout = 0;
  char ym2151_type[8] = "fmgen";

  int opt;
  // Handle long options
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      video_filename = argv[i + 1];
      enable_visualizer = true;
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--video-fps") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      video_fps = atoi(argv[i + 1]);
      if (video_fps < 1 || video_fps > 120) {
        fprintf(stderr, "Video FPS must be between 1 and 120.\n");
        return -1;
      }
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      screenshot_filename = argv[i + 1];
      enable_visualizer = true;
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--swap-channels") == 0) {
      swap_channels = true;
      // Remove this arg from argv
      for (int j = i; j < argc - 1; j++) {
        argv[j] = argv[j + 1];
      }
      argc--;
      i--;
    } else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
      volume = atof(argv[i + 1]);
      if (volume < 0.0f || volume > 2.0f) {
        fprintf(stderr, "Volume must be between 0.0 and 2.0.\n");
        return -1;
      }
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
    } else if (strcmp(argv[i], "--ym2151-ch") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      ym2151_channels = atoi(argv[i + 1]);
      if (ym2151_channels < 1 || ym2151_channels > 8) {
        fprintf(stderr, "YM2151 channels must be between 1 and 8.\n");
        return -1;
      }
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--adpcm-ch") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      adpcm_channels = atoi(argv[i + 1]);
      if (adpcm_channels < 0 || adpcm_channels > 8) {
        fprintf(stderr, "ADPCM channels must be between 0 and 8.\n");
        return -1;
      }
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--ym2151-waveform-scale") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      ym2151_waveform_scale = atof(argv[i + 1]);
      if (ym2151_waveform_scale < 0.1f || ym2151_waveform_scale > 10.0f) {
        fprintf(stderr, "YM2151 waveform scale must be between 0.1 and 10.0.\n");
        return -1;
      }
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--adpcm-waveform-scale") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      adpcm_waveform_scale = atof(argv[i + 1]);
      if (adpcm_waveform_scale < 0.1f || adpcm_waveform_scale > 10.0f) {
        fprintf(stderr, "ADPCM waveform scale must be between 0.1 and 10.0.\n");
        return -1;
      }
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--waveform-scale") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      float scale = atof(argv[i + 1]);
      if (scale < 0.1f || scale > 10.0f) {
        fprintf(stderr, "Waveform scale must be between 0.1 and 10.0.\n");
        return -1;
      }
      ym2151_waveform_scale = scale;
      adpcm_waveform_scale = scale;
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    } else if (strcmp(argv[i], "--spectrum-debug") == 0 && i + 1 < argc) {
#ifdef ENABLE_VISUALIZER
      spectrum_debug_filename = argv[i + 1];
      // Remove these args from argv
      for (int j = i; j < argc - 2; j++) {
        argv[j] = argv[j + 2];
      }
      argc -= 2;
      i--;
      enable_visualizer = true;
#else
      fprintf(stderr, "Visualizer support is not enabled.\n");
      return -1;
#endif
    }
  }
  
  while ((opt = getopt(argc, argv, "d:e:fgl:mpr:tvV")) != -1) {
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
      case 'g':
#ifdef ENABLE_VISUALIZER
        enable_visualizer = true;
#else
        fprintf(stderr, "Visualizer support is not enabled. Rebuild with -DENABLE_VISUALIZER=ON\n");
        return -1;
#endif
        break;
      case 'l':
        loop = atoi(optarg);
        break;
      case 'm':
        measure_play_time = true;
        break;
      case 'p':
#ifdef __APPLE__
        play_audio = true;
#else
        fprintf(stderr, "Audio playback (-p) is only supported on macOS.\n");
        return -1;
#endif
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

  // Check for conflicting options
  if (play_audio && video_filename) {
    fprintf(stderr, "Error: -p and --video options cannot be used together.\n");
    return -1;
  }

  const char *mdx_name = argv[optind];
  if (mdx_name == 0 || *mdx_name == 0) {
    help();
    return 0;
  }
  
  // ディレクトリが指定された場合の処理（ビジュアライザー無効時）
#ifndef ENABLE_VISUALIZER
  struct stat path_stat;
  if (stat(mdx_name, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
    std::vector<std::string> file_list = listMDXFiles(mdx_name);
    if (!file_list.empty()) {
      mdx_name = file_list[0].c_str();
      fprintf(stderr, "Directory specified. Playing first file: %s\n", mdx_name);
    } else {
      fprintf(stderr, "No MDX files found in directory: %s\n", mdx_name);
      return -1;
    }
  }
#endif

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
  std::vector<const char*> file_ptrs;
  int current_file_index = 0;
  
  if (enable_visualizer) {
    // ディレクトリ内のMDXファイルを列挙
    file_list = listMDXFiles(mdx_name);
    
    // ディレクトリが指定された場合、最初のファイルを再生
    struct stat path_stat;
    if (stat(mdx_name, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      if (!file_list.empty()) {
        mdx_name = file_list[0].c_str();
        current_file_index = 0;
        fprintf(stderr, "Directory specified. Playing first file: %s\n", mdx_name);
      } else {
        fprintf(stderr, "No MDX files found in directory: %s\n", mdx_name);
        return -1;
      }
    } else {
      // 現在のファイルのインデックスを見つける
      std::string current_file(mdx_name);
      for (size_t i = 0; i < file_list.size(); i++) {
        if (file_list[i] == current_file) {
          current_file_index = i;
          break;
        }
      }
    }
    
    // YM2151状態管理オブジェクト作成
    ym_state = new YM2151State();
    
    // ウィンドウの高さを計算（表示チャンネル数に応じて調整）
    // タイトル: 50px, Timer情報: 15px, 区切り線: 10px = 75px
    // YM2151チャンネル: 70px/ch
    // ADPCMチャンネル: 45px/ch
    // 下部余白: 20px
    int window_height = 75 + (ym2151_channels * 70) + (adpcm_channels * 45) + 20;
    
    // 動画モードの場合は高さを偶数に調整（H.264要件）
    if (video_filename && (window_height % 2) != 0) {
      window_height++;
    }
    
    // ビジュアライザー初期化
    visualizer = new Visualizer();
    bool init_success = false;
    
    if (video_filename) {
      // 動画録画モード
      init_success = visualizer->initVideoMode(video_filename, 1400, window_height, video_fps, SAMPLE_RATE);
      if (!init_success) {
        fprintf(stderr, "Failed to initialize visualizer in video mode\n");
        delete visualizer;
        delete ym_state;
        return -1;
      }
      fprintf(stderr, "Video recording mode: %s (%dfps)\n", video_filename, video_fps);
    } else {
      // 通常の表示モード
      init_success = visualizer->init("MDX Visualizer", 1400, window_height);
      if (!init_success) {
        fprintf(stderr, "Failed to initialize visualizer\n");
        delete visualizer;
        delete ym_state;
        return -1;
      }
    }
    
    visualizer->setState(ym_state);
    
    // 表示チャンネル数を設定
    visualizer->setDisplayChannels(ym2151_channels, adpcm_channels);
    
    // 波形スケールを設定
    visualizer->setWaveformScale(ym2151_waveform_scale, adpcm_waveform_scale);
    
    // スペクトラムデバッグモードを設定
    if (spectrum_debug_filename) {
      visualizer->setSpectrumDebug(spectrum_debug_filename);
    }
    
    // ファイルリストを設定
    if (!file_list.empty()) {
      for (const auto& f : file_list) {
        file_ptrs.push_back(f.c_str());
      }
      visualizer->setFileList(file_ptrs.data(), file_ptrs.size(), current_file_index);
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

  MXDRVG_Start(SAMPLE_RATE, filter_mode, MDX_BUF_SIZE, PDX_BUF_SIZE);
  MXDRVG_TotalVolume(256);
  
  // visualizerモードでは連続再生を有効化
  g_continuous_playback = enable_visualizer;
  
  short *audio_buf = new short [AUDIO_BUF_SAMPLES * 2];

#ifdef __APPLE__
  AudioContext audioCtx = {0};
  if (play_audio) {
    // Direct audio playback mode
    if (!initAudioQueue(&audioCtx, SAMPLE_RATE, AUDIO_BUF_SAMPLES)) {
      fprintf(stderr, "Failed to initialize audio playback.\n");
      MXDRVG_End();
      delete []audio_buf;
      return -1;
    }
    
    if (verbose) {
      fprintf(stderr, "Playing audio... (Press Ctrl+C to stop)\n");
    }
  }
#endif

reload_file:
  // ファイル切り替え時はMXDRVGのみリセット
  MXDRVG_End();
  MXDRVG_Start(SAMPLE_RATE, filter_mode, MDX_BUF_SIZE, PDX_BUF_SIZE);
  MXDRVG_TotalVolume(256);
  
  char title[256];
  unsigned int pdx_size = 0;

  if (!LoadMDX(mdx_name, title, sizeof(title), &pdx_size)) {
    return -1;
  }

#ifdef ENABLE_VISUALIZER
  // ビジュアライザーにタイトルとファイル名を設定
  if (visualizer && ym_state) {
    visualizer->setSongTitle(title);
    visualizer->setFilename(mdx_name);
    visualizer->resetElapsedTime();
    
    // ファイルリストのインデックスを更新
    if (!file_list.empty()) {
      std::vector<const char*> file_ptrs;
      for (const auto& f : file_list) {
        file_ptrs.push_back(f.c_str());
      }
      visualizer->setFileList(file_ptrs.data(), file_ptrs.size(), current_file_index);
    }
    
    // ADPCM全体バッファ情報を取得して設定
    void* adpcm_buffer = MXDRVG_GetADPCMBuffer();
    ym_state->setADPCMBufferInfo(adpcm_buffer, pdx_size);
    
    if (verbose) {
      fprintf(stderr, "ADPCM buffer: %p, size: %u bytes (PDX data size)\n", adpcm_buffer, pdx_size);
    }
  }
#endif

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

  {
    if (max_song_duration < song_duration) {
      song_duration = max_song_duration;
    }
  }

  // Main loop
  double total_audio_time = 0.0;  // 動画モード用: 累積オーディオ時間
  int video_frames_rendered = 0;   // 動画モード用: レンダリング済みフレーム数
  
  for (int i = 0; song_duration == 0.0f || 1.0f * i * AUDIO_BUF_SAMPLES / SAMPLE_RATE < song_duration; i++) {
#ifdef ENABLE_VISUALIZER
    // 動画モード: 累積オーディオ時間で正確に制御
    if (song_duration > 0.0f && total_audio_time >= song_duration) {
      break;
    }
    
    // fmgenからEG情報を更新
    if (enable_visualizer && ym_state && opm_original) {
      void* opm_ptr = opm_original->GetOPMPointer();
      if (opm_ptr) {
        ym_state->updateFromFmgen(opm_ptr);
        // visualizerにOPMポインタを設定（Timerトグル用）
        if (visualizer) {
          visualizer->setOPMPointer(opm_ptr);
        }
      }
    }
    
    // Update visualizer (must be on main thread for macOS)
    if (enable_visualizer && visualizer && !video_filename) {
      // 通常モード: 既存の処理
      // MXDRVGの一時停止状態を適用
      static bool last_pause_state = false;
      bool current_pause_state = visualizer->isMXDRVGPaused();
      if (current_pause_state != last_pause_state) {
        if (current_pause_state) {
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
          MXDRVG_End();
          goto reload_file;
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
            MXDRVG_End();
            goto reload_file;
          }
        }
        
        if (!visualizer->isRunning()) {
          break;
        }
    }
#endif

    // ビジュアライザー使用時は曲の終了を無視（ファイル切り替えや手動終了まで継続）
    // ただし動画録画モードの場合は曲終了を検知して停止
#ifdef ENABLE_VISUALIZER
    if (!enable_visualizer || video_filename)
#endif
    {
      if (MXDRVG_GetTerminated()) {
        break;
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
      
      // ビジュアライザー使用時は曲の終了でも停止しない（duration指定がない場合）
      if (!enable_visualizer)
#endif
      {
        if (audioCtx.terminated) {
          fprintf(stderr, "Audio playback terminated.\n");
          break;
        }
      }
      SDL_Delay(16); // ~60fps for visualizer
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
    
    int len = MXDRVG_GetPCM(audio_buf, samples_to_get);
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
  if (play_audio) {
    cleanupAudioQueue(&audioCtx);
  }
#endif

  delete []audio_buf;

  MXDRVG_End();

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
