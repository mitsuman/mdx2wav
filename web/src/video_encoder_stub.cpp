// Web build stub for the FFmpeg based video encoder.
//
// The browser port renders to a canvas instead of encoding MP4 files, so the
// video path is unused.  This stub keeps visualizer.cpp linkable without
// pulling FFmpeg into the WebAssembly build.

#include "src/visualizer/video_encoder.h"

VideoEncoder::VideoEncoder()
    : format_ctx_(nullptr), video_codec_ctx_(nullptr), audio_codec_ctx_(nullptr),
      video_stream_(nullptr), audio_stream_(nullptr), video_frame_(nullptr),
      audio_frame_(nullptr), packet_(nullptr), sws_ctx_(nullptr),
      width_(0), height_(0), fps_(0), sample_rate_(0), audio_channels_(0),
      video_pts_(0), audio_pts_(0), initialized_(false),
      audio_buffer_(nullptr), audio_buffer_size_(0), audio_buffer_pos_(0) {}

VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::init(const char* filename, int width, int height, int fps,
                        int sample_rate, int audio_channels) {
  (void)filename;
  (void)width;
  (void)height;
  (void)fps;
  (void)sample_rate;
  (void)audio_channels;
  error_message_ = "video encoding is not available in the browser build";
  return false;
}

bool VideoEncoder::addVideoFrame(const unsigned char* rgb_data) {
  (void)rgb_data;
  return false;
}

bool VideoEncoder::addAudioSamples(const short* samples, int sample_count) {
  (void)samples;
  (void)sample_count;
  return false;
}

void VideoEncoder::finalize() {}
