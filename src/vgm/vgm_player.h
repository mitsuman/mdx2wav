#ifndef MDX2WAV_VGM_PLAYER_H
#define MDX2WAV_VGM_PLAYER_H

#include <stdint.h>

#include <string>
#include <vector>

#include "../../gamdx/opn2/ym3438.h"
#include "sn76489.h"

// libvgm provides the actual VGM playback (accurate chip cores and timing);
// our own parser and OPN2 instance are only used for the visualizer state.
class LibVgmAudio;

// Plays Sega Genesis style VGM files: YM2612 (Nuked OPN2) plus SN76489 PSG.
class VgmFilePlayer {
 public:
  enum { kFmChannels = 6, kWaveformSamples = 1024 };

  struct VoiceState {
    bool active;
    int note;           // MIDI note, -1 when unknown
    int fnum, block;
    int algorithm, feedback;
    int pan_left, pan_right;
    int eg_out[4];      // operator attenuation (0..1023, 0 = loudest)
    int key_on;
    int16_t waveform[kWaveformSamples];
    int waveform_samples;
  };

  ~VgmFilePlayer();

  bool Load(const char* path, std::string* error);
  void Start(int loop, double max_duration, double fade_seconds);
  int Generate(int16_t* out, int frames);
  bool IsFinished() const { return finished_; }
  double Duration() const { return duration_; }
  const char* Title() const { return title_.c_str(); }
  int SampleRate() const { return 44100; }
  void GetVoiceStates(VoiceState out[kFmChannels]) const;
  void GetPsgStates(SN76489::ChannelState out[SN76489::kChannels]) const;

  struct DacState {
    bool enabled;
    int16_t waveform[kWaveformSamples];
    int waveform_samples;
  };
  void GetDacState(DacState* out) const;
  int FmChannelCount() const { return has_opn2_ ? kFmChannels : 0; }
  bool HasPsg() const { return has_psg_; }

 private:
  bool RunCommand();
  void WriteOpn2(int port, uint8_t data);
  void ClockChips();

  std::vector<uint8_t> data_;
  uint32_t data_offset_ = 0;
  uint32_t loop_offset_ = 0;
  uint32_t total_samples_ = 0;
  uint32_t loop_samples_ = 0;
  uint32_t pos_ = 0;
  int wait_ = 0;

  ym3438_t opn2_;
  bool has_opn2_ = false;
  int opn2_clock_ = 7670453;
  SN76489 psg_;
  bool has_psg_ = false;

  double chip_phase_ = 0.0;
  int pending_skip_ = 0;   // chip cycles already spent while latching writes
  int16_t last_l_ = 0, last_r_ = 0;
  double dc_l_ = 0.0, dc_r_ = 0.0;   // DC blocker state (YM2612 DAC bias)
  double elapsed_ = 0.0;
  double duration_ = 0.0;
  double fade_ = 0.0;
  int loop_ = 1;
  bool finished_ = false;
  std::string title_;

  LibVgmAudio* audio_ = nullptr;
  std::vector<int16_t> lib_audio_;

  int16_t wave_ring_[kFmChannels][kWaveformSamples];
  uint32_t wave_pos_[kFmChannels];
};

#endif
