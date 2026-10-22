#ifndef SPC_PLAYER_H
#define SPC_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "spc_resampler.h"

struct SNES_SPC;
struct SPC_Filter;

// Plays an SNES .spc file through Blargg's SPC700 + S-DSP emulator and exposes
// the live DSP state for the visualizer.
//
// SPC files contain the APU's 64 KB RAM and the CPU/DSP registers; the music
// program itself keeps running and loops forever.  There is no reliable song
// length in the format, so playback stops after the ID666 length (when present)
// or after the caller-supplied maximum duration.  The `loop` argument is
// accepted for command-line compatibility but the SPC program owns looping.
class SPCPlayer {
 public:
  static const int kVoices = 8;
  static const int kWaveformSamples = 4096;

  struct VoiceState {
    bool key_on;
    bool kon;
    int note;            // MIDI-style note derived from the pitch register
    int pitch;           // 14-bit pitch register (0x1000 = 32 kHz)
    int volume;          // 0..127, envelope and voice volume combined
    int left_volume;     // signed voice volume (-128..127)
    int right_volume;
    int envelope;        // 0..0x7FF
    int envelope_phase;  // 0 release, 1 attack, 2 decay, 3 sustain
    int sample_rate;     // effective playback rate in Hz
    int source_number;   // SRCN register
    int source_address;  // DIR entry start address
    int loop_address;    // DIR entry loop address
    int brr_address;     // current BRR block address
    bool looped;         // decoded stream has the BRR loop flag
    int loop_samples;    // samples in one BRR loop (0 if unknown)
    int waveform_samples;
    int16_t waveform[kWaveformSamples];
  };

  struct GlobalState {
    int master_volume_left;
    int master_volume_right;
    int echo_volume_left;
    int echo_volume_right;
    int echo_feedback;
    int echo_delay;       // EDL, in 16 ms units
    int echo_start;       // ESA, in 256 byte units
    int echo_offset;
    int echo_length;
    int noise_enable;
    int key_on_mask;
    int directory_address;
    int cpu_pc;
  };

  SPCPlayer();
  ~SPCPlayer();

  // Validates and loads an SPC image.  On failure `error` receives a message.
  bool Load(const uint8_t* data, size_t size, std::string* error);

  // Output rate for Generate().  Call before Start(); defaults to 44100.
  void SetSampleRate(int rate);

  // Configure playback.  `max_duration` is in seconds; 0 means no limit.
  // `fadeout_seconds` is applied over the last part of the duration.
  void Start(int loop, double max_duration, double fadeout_seconds);

  // Render `frames` stereo frames at the configured sample rate.
  int Generate(int16_t* out, int frames);

  bool IsLoaded() const { return loaded_; }
  bool IsFinished() const { return finished_; }
  double Duration() const { return duration_; }
  double Elapsed() const { return elapsed_; }
  int LoopCount() const { return loop_; }
  int SampleRate() const { return sample_rate_; }

  // ID666 tag fields (Shift-JIS bytes as stored in the file, NUL terminated).
  const char* Title() const { return title_.c_str(); }
  const char* Game() const { return game_.c_str(); }
  const char* Artist() const { return artist_.c_str(); }
  const char* DumpDate() const { return dump_date_.c_str(); }

  // Snapshot of the emulator state for the visualizer.
  void GetVoiceStates(VoiceState out[kVoices]) const;
  void GetGlobalState(GlobalState* out) const;

  static bool IsSPCFile(const uint8_t* data, size_t size);

 private:
  bool GenerateInput();
  void ParseId666(const uint8_t* data, size_t size);
  void ApplyFadeout(int16_t* out, int frames);

  SNES_SPC* spc_;
  SPC_Filter* filter_;
  SPCResampler resampler_;

  bool loaded_;
  bool finished_;
  int sample_rate_;
  int loop_;
  double max_duration_;
  double duration_;
  double fadeout_seconds_;
  double elapsed_;
  double id666_seconds_;

  std::string title_;
  std::string game_;
  std::string artist_;
  std::string dump_date_;

  std::vector<int16_t> raw_input_;
  std::vector<uint8_t> spc_data_;  // original image, used to restart playback
};

#endif  // SPC_PLAYER_H
