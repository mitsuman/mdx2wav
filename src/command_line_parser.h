#pragma once

#include <string>

struct CommandLineOptions {
  CommandLineOptions();

  int mdx_buf_size = 256 * 1024;
  int pdx_buf_size = 1024 * 1024;
  int sample_rate = 44100;
  int filter_mode = 0;
  bool measure_play_time = false;
  bool get_title = false;
  bool play_audio = false;
  bool enable_visualizer = false;
  bool swap_channels = false;
  bool show_help = false;
  bool show_version = false;
  bool verbose_logging = false;
  float volume = 1.0f;
  float ym2151_waveform_scale = 1.0f;
  float adpcm_waveform_scale = 1.0f;
  std::string screenshot_filename;
  std::string spectrum_debug_filename;
  std::string video_filename;
  int video_fps = 60;
  int ym2151_channels = 8;
  int adpcm_channels = 8;
  float max_song_duration = 300.0f;
  int loop = 2;
  int fadeout = 0;
  char ym2151_type[8];
  std::string mdx_input;
};

class CommandLineParser {
 public:
  CommandLineParser(int argc, char** argv);

  bool Parse(CommandLineOptions* options, std::string* error_message);

 private:
  static bool ParseFloat(const std::string& text, float* out);
  static bool ParseInt(const std::string& text, int* out);

  bool HandleLongOption(const std::string& arg, int& index, CommandLineOptions* options, std::string* error_message);
  bool HandleShortOptions(const std::string& token, int& index, CommandLineOptions* options, std::string* error_message);

  int argc_ = 0;
  char** argv_ = nullptr;
};
