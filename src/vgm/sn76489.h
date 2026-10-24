#ifndef MDX2WAV_SN76489_H
#define MDX2WAV_SN76489_H

#include <stdint.h>

// Minimal SN76489 (Sega PSG) used by VGM playback: three tone channels and one
// noise channel, 4-bit attenuation, 16-step volume.
class SN76489 {
 public:
  SN76489();

  enum { kChannels = 4, kWaveformSamples = 1024 };

  struct ChannelState {
    bool active;
    int note;        // MIDI note (tone channels), -1 for noise
    int period;
    int volume;      // attenuation 0..15
    bool white_noise; // noise channel: white (true) or periodic (false)
    int16_t waveform[kWaveformSamples];
  };

  void Reset(int clock);
  void Write(uint8_t data);
  void GetStates(ChannelState out[kChannels]) const;
  // Adds one sample (at 44100 Hz) to the left/right accumulators.
  void Generate(int16_t* left, int16_t* right);

 private:
  struct Tone {
    int period = 0x3FF;
    int volume = 15;   // attenuation, 0 = loudest
    double phase = 0.0;
    int output = 0;
  };

  double StepForPeriod(int period) const;
  int Amplitude(int attenuation) const;

  int clock_ = 3579545;
  int latch_ = 0;
  bool latched_ = false;
  Tone tone_[3];
  Tone noise_;
  int16_t wave_[kChannels][kWaveformSamples];
  uint32_t wave_pos_[kChannels] = {0, 0, 0, 0};
  uint16_t lfsr_ = 0x8000;
  int noise_shift_ = 0;
  bool noise_white_ = false;
};

#endif
