#include "command_line_parser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

CommandLineOptions::CommandLineOptions() {
  snprintf(ym2151_type, sizeof(ym2151_type), "fmgen");
}

CommandLineParser::CommandLineParser(int argc, char** argv)
  : argc_(argc), argv_(argv) {}

bool CommandLineParser::Parse(CommandLineOptions* options, std::string* error_message) {
  if (!options || !error_message) {
    return false;
  }

  std::vector<std::string> positional;
  for (int i = 1; i < argc_; ++i) {
    const char* raw_arg = argv_[i];
    if (!raw_arg) {
      continue;
    }
    std::string arg(raw_arg);
    if (arg == "--") {
      for (int j = i + 1; j < argc_; ++j) {
        positional.emplace_back(argv_[j]);
      }
      break;
    }
    if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
      if (!HandleLongOption(arg, i, options, error_message)) {
        return false;
      }
      continue;
    }
    if (arg.size() > 1 && arg[0] == '-') {
      if (!HandleShortOptions(arg, i, options, error_message)) {
        return false;
      }
      continue;
    }
    positional.emplace_back(arg);
  }

  if (!positional.empty()) {
    options->mdx_input = positional.front();
  }

  return true;
}

bool CommandLineParser::ParseFloat(const std::string& text, float* out) {
  if (!out) {
    return false;
  }
  char* end = nullptr;
  float value = strtof(text.c_str(), &end);
  if (end == text.c_str() || (end && *end != '\0')) {
    return false;
  }
  *out = value;
  return true;
}

bool CommandLineParser::ParseInt(const std::string& text, int* out) {
  if (!out) {
    return false;
  }
  char* end = nullptr;
  long value = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || (end && *end != '\0')) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

bool CommandLineParser::HandleLongOption(const std::string& arg, int& index, CommandLineOptions* options, std::string* error_message) {
  size_t eq_pos = arg.find('=');
  std::string name = arg.substr(2, eq_pos == std::string::npos ? std::string::npos : eq_pos - 2);
  std::string value;
  if (eq_pos != std::string::npos) {
    value = arg.substr(eq_pos + 1);
  }

  auto require_value = [&](std::string* out) -> bool {
    if (!out) {
      return false;
    }
    if (!value.empty()) {
      *out = value;
      value.clear();
      return true;
    }
    if (index + 1 >= argc_) {
      *error_message = "Missing value for option --" + name;
      return false;
    }
    *out = argv_[++index];
    return true;
  };

  if (name == "help") {
    options->show_help = true;
    return true;
  }
  if (name == "volume") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    float val = atof(str.c_str());
    if (val < 0.0f || val > 2.0f) {
      *error_message = "Volume must be between 0.0 and 2.0.";
      return false;
    }
    options->volume = val;
    return true;
  }
  if (name == "swap-channels") {
    options->swap_channels = true;
    return true;
  }
#ifdef ENABLE_VISUALIZER
  if (name == "video") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    options->video_filename = str;
    options->enable_visualizer = true;
    return true;
  }
  if (name == "video-fps") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    int fps = 0;
    if (!ParseInt(str, &fps) || fps < 1 || fps > 120) {
      *error_message = "Video FPS must be between 1 and 120.";
      return false;
    }
    options->video_fps = fps;
    return true;
  }
  if (name == "screenshot") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    options->screenshot_filename = str;
    options->enable_visualizer = true;
    return true;
  }
  if (name == "ym2151-ch") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    int value_int = 0;
    if (!ParseInt(str, &value_int) || value_int < 1 || value_int > 8) {
      *error_message = "YM2151 channels must be between 1 and 8.";
      return false;
    }
    options->ym2151_channels = value_int;
    return true;
  }
  if (name == "adpcm-ch") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    int value_int = 0;
    if (!ParseInt(str, &value_int) || value_int < 0 || value_int > 8) {
      *error_message = "ADPCM channels must be between 0 and 8.";
      return false;
    }
    options->adpcm_channels = value_int;
    return true;
  }
  if (name == "ym2151-waveform-scale") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    float val = 0.0f;
    if (!ParseFloat(str, &val) || val < 0.1f || val > 10.0f) {
      *error_message = "YM2151 waveform scale must be between 0.1 and 10.0.";
      return false;
    }
    options->ym2151_waveform_scale = val;
    return true;
  }
  if (name == "adpcm-waveform-scale") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    float val = 0.0f;
    if (!ParseFloat(str, &val) || val < 0.1f || val > 10.0f) {
      *error_message = "ADPCM waveform scale must be between 0.1 and 10.0.";
      return false;
    }
    options->adpcm_waveform_scale = val;
    return true;
  }
  if (name == "waveform-scale") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    float val = 0.0f;
    if (!ParseFloat(str, &val) || val < 0.1f || val > 10.0f) {
      *error_message = "Waveform scale must be between 0.1 and 10.0.";
      return false;
    }
    options->ym2151_waveform_scale = val;
    options->adpcm_waveform_scale = val;
    return true;
  }
  if (name == "spectrum-debug") {
    std::string str;
    if (!require_value(&str)) {
      return false;
    }
    options->spectrum_debug_filename = str;
    options->enable_visualizer = true;
    return true;
  }
#else
  if (name == "video" || name == "video-fps" || name == "screenshot" ||
      name == "ym2151-ch" || name == "adpcm-ch" ||
      name == "ym2151-waveform-scale" || name == "adpcm-waveform-scale" ||
      name == "waveform-scale" || name == "spectrum-debug") {
    *error_message = "Visualizer support is not enabled.";
    return false;
  }
#endif

  *error_message = "Unknown option --" + name;
  return false;
}

bool CommandLineParser::HandleShortOptions(const std::string& token, int& index, CommandLineOptions* options, std::string* error_message) {
  size_t pos = 1;
  while (pos < token.size()) {
    char flag = token[pos];
    auto require_value = [&](std::string* out) -> bool {
      if (!out) {
        return false;
      }
      if (pos + 1 < token.size()) {
        *out = token.substr(pos + 1);
        pos = token.size();
        return true;
      }
      if (index + 1 >= argc_) {
        *error_message = std::string("Missing value for option -") + flag;
        return false;
      }
      *out = argv_[++index];
      return true;
    };

    switch (flag) {
      case 'h':
        options->show_help = true;
        return true;
      case 'd': {
        std::string str;
        if (!require_value(&str)) {
          return false;
        }
        options->max_song_duration = atof(str.c_str());
        break;
      }
      case 'e': {
        std::string str;
        if (!require_value(&str)) {
          return false;
        }
        snprintf(options->ym2151_type, sizeof(options->ym2151_type), "%s", str.c_str());
        break;
      }
      case 'f':
        options->fadeout = 1;
        break;
      case 'g':
#ifdef ENABLE_VISUALIZER
        options->enable_visualizer = true;
        break;
#else
        *error_message = "Visualizer support is not enabled. Rebuild with -DENABLE_VISUALIZER=ON";
        return false;
#endif
      case 'l': {
        std::string str;
        if (!require_value(&str)) {
          return false;
        }
        options->loop = atoi(str.c_str());
        break;
      }
      case 'm':
        options->measure_play_time = true;
        break;
      case 'p':
#ifdef __APPLE__
        options->play_audio = true;
        break;
#else
        *error_message = "Audio playback (-p) is only supported on macOS.";
        return false;
#endif
      case 'r': {
        std::string str;
        if (!require_value(&str)) {
          return false;
        }
        options->sample_rate = atoi(str.c_str());
        break;
      }
      case 't':
        options->get_title = true;
        break;
      case 'v':
        options->show_version = true;
        return true;
      case 'V':
        options->verbose_logging = true;
        break;
      default:
        *error_message = std::string("Unknown option -") + flag;
        return false;
    }
    ++pos;
  }
  return true;
}
