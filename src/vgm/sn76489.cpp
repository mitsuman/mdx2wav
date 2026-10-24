#include "sn76489.h"

SN76489::SN76489() {
  Reset(3579545);
}

void SN76489::Reset(int clock) {
  clock_ = clock > 0 ? clock : 3579545;
  for (int i = 0; i < 3; i++) {
    tone_[i] = Tone();
    tone_[i].volume = 15;
  }
  noise_ = Tone();
  noise_.volume = 15;
  lfsr_ = 0x8000;
  noise_shift_ = 0;
  noise_white_ = false;
  latch_ = 0;
  latched_ = false;
}

double SN76489::StepForPeriod(int period) const {
  if (period < 1) period = 1;
  // Tone frequency = clock / (32 * period); one sample is 1/44100 s.
  return (double)clock_ / (32.0 * (double)period * 44100.0);
}

int SN76489::Amplitude(int attenuation) const {
  if (attenuation >= 15) return 0;
  // 2 dB per step; 0x2000 at full volume keeps it in range with the FM mix.
  static const int table[16] = {
      8192, 6500, 5160, 4096, 3250, 2580, 2048, 1626,
      1290, 1024, 813, 645, 512, 406, 322, 0};
  return table[attenuation & 15];
}

void SN76489::Write(uint8_t data) {
  if (data & 0x80) {
    latch_ = data & 0x70;
    latched_ = true;
    const int ch = (data >> 5) & 3;
    const bool is_volume = (data & 0x10) != 0;
    if (ch < 3) {
      Tone& t = tone_[ch];
      if (is_volume) {
        t.volume = data & 0x0F;
      } else {
        t.period = (t.period & 0x3F0) | (data & 0x0F);
      }
    } else {
      if (is_volume) {
        noise_.volume = data & 0x0F;
      } else {
        noise_shift_ = data & 0x03;
        noise_white_ = (data & 0x04) != 0;
        lfsr_ = 0x8000;
      }
    }
    return;
  }
  if (!latched_) return;
  const int ch = (latch_ >> 5) & 3;
  const bool is_volume = (latch_ & 0x10) != 0;
  if (ch < 3) {
    Tone& t = tone_[ch];
    if (is_volume) {
      t.volume = data & 0x0F;
    } else {
      t.period = (t.period & 0x00F) | ((data & 0x3F) << 4);
    }
  } else {
    if (is_volume) {
      noise_.volume = data & 0x0F;
    } else {
      noise_shift_ = data & 0x03;
      noise_white_ = (data & 0x04) != 0;
      lfsr_ = 0x8000;
    }
  }
}

void SN76489::Generate(int16_t* left, int16_t* right) {
  int mix = 0;

  for (int i = 0; i < 3; i++) {
    Tone& t = tone_[i];
    t.phase += StepForPeriod(t.period);
    while (t.phase >= 1.0) {
      t.phase -= 1.0;
      t.output ^= 1;
    }
    if (t.output) mix += Amplitude(t.volume);
  }

  // Noise: the shift rate follows the selected mode.
  {
    int period = 0x10;
    if (noise_shift_ == 0) period = 0x10;
    else if (noise_shift_ == 1) period = 0x20;
    else if (noise_shift_ == 2) period = 0x40;
    else period = tone_[2].period ? tone_[2].period : 1;
    noise_.phase += StepForPeriod(period);
    while (noise_.phase >= 1.0) {
      noise_.phase -= 1.0;
      const uint16_t bit = (lfsr_ ^ (lfsr_ >> 1)) & 1;
      lfsr_ = (lfsr_ >> 1) | (bit << 15);
      if (noise_white_) {
        noise_.output = lfsr_ & 1;
      } else {
        noise_.output = (lfsr_ & 1) ? 1 : 0;
        // Periodic mode: retrigger the LFSR at the end of its period.
        if (lfsr_ == 1) lfsr_ = 0x8000;
      }
    }
    if (noise_.output) mix += Amplitude(noise_.volume);
  }

  if (mix > 32767) mix = 32767;
  if (mix < -32768) mix = -32768;
  *left = (int16_t)mix;
  *right = (int16_t)mix;
}
