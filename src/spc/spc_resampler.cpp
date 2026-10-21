#include "spc_resampler.h"

#include <math.h>
#include <string.h>
#include <algorithm>

SPCResampler::SPCResampler()
    : configured_(false),
      in_rate_(0),
      out_rate_(0),
      step_(1.0),
      buffer_frames_(0),
      position_(0.0) {}

void SPCResampler::Init(int in_rate, int out_rate) {
  if (in_rate <= 0) in_rate = 1;
  if (out_rate <= 0) out_rate = 1;
  in_rate_ = in_rate;
  out_rate_ = out_rate;
  step_ = (double)in_rate_ / (double)out_rate_;
  BuildCoefficients();
  configured_ = true;
  Reset();
}

void SPCResampler::Reset() {
  buffer_.clear();
  buffer_frames_ = 0;
  position_ = 0.0;
}

void SPCResampler::BuildCoefficients() {
  // Low-pass at the lower of the two Nyquist frequencies.  The 0.95 factor
  // leaves a little room for the filter's transition band.  When upsampling
  // this is simply the input Nyquist, i.e. a mild reconstruction filter.
  const double pi = 3.14159265358979323846;
  const double ratio = (double)out_rate_ / (double)in_rate_;
  double cutoff = 0.5 * (ratio < 1.0 ? ratio : 1.0) * 0.95;
  if (cutoff <= 0.0) cutoff = 0.05;

  const int half = kTaps / 2;
  coeffs_.assign((kPhases + 1) * kTaps, 0.0f);

  for (int phase = 0; phase <= kPhases; phase++) {
    const double frac = (double)phase / (double)kPhases;
    float* c = &coeffs_[phase * kTaps];
    double sum = 0.0;

    for (int n = 0; n < kTaps; n++) {
      // Distance from the output position to this input sample.  The sinc is
      // centred between the two nearest samples for fractional positions.
      const double x = (double)(n - half) - frac;
      const double arg = 2.0 * pi * cutoff * x;
      double sinc = (fabs(arg) < 1e-9) ? 1.0 : sin(arg) / arg;
      // Hann window, also shifted to follow the fractional position.
      const double wpos = ((double)n - (double)half - frac) / (double)half;
      double window = 0.0;
      if (wpos > -1.0 && wpos < 1.0) {
        window = 0.5 + 0.5 * cos(pi * wpos);
      }
      const double v = sinc * window * 2.0 * cutoff;
      c[n] = (float)v;
      sum += v;
    }

    if (fabs(sum) > 1e-9) {
      const float inv = (float)(1.0 / sum);
      for (int n = 0; n < kTaps; n++) {
        c[n] *= inv;
      }
    }
  }
}

void SPCResampler::Push(const int16_t* frames, int frame_count) {
  if (!configured_ || !frames || frame_count <= 0) return;

  buffer_.resize((size_t)(buffer_frames_ + frame_count) * 2);
  float* dst = &buffer_[(size_t)buffer_frames_ * 2];
  for (int i = 0; i < frame_count * 2; i++) {
    dst[i] = (float)frames[i];
  }
  buffer_frames_ += frame_count;
}

int SPCResampler::Pull(int16_t* out, int max_frames) {
  if (!configured_ || !out || max_frames <= 0) return 0;

  const int half = kTaps / 2;

  int written = 0;
  while (written < max_frames) {
    const int base = (int)floor(position_);
    // We need `half` frames of look-ahead for the kernel.
    if (base + half >= buffer_frames_) break;

    double frac = position_ - base;
    if (frac < 0.0) frac += 1.0;
    int phase = (int)(frac * kPhases + 0.5);
    if (phase > kPhases) phase = kPhases;
    const float* c = &coeffs_[phase * kTaps];

    float l = 0.0f;
    float r = 0.0f;
    for (int n = 0; n < kTaps; n++) {
      int idx = base - half + n;
      if (idx < 0) idx = 0;
      if (idx >= buffer_frames_) idx = buffer_frames_ - 1;
      const float* s = &buffer_[(size_t)idx * 2];
      l += s[0] * c[n];
      r += s[1] * c[n];
    }

    int il = (int)(l >= 0.0f ? l + 0.5f : l - 0.5f);
    int ir = (int)(r >= 0.0f ? r + 0.5f : r - 0.5f);
    if (il > 32767) il = 32767;
    else if (il < -32768) il = -32768;
    if (ir > 32767) ir = 32767;
    else if (ir < -32768) ir = -32768;
    out[written * 2] = (int16_t)il;
    out[written * 2 + 1] = (int16_t)ir;
    written++;

    position_ += step_;
  }

  // Drop history that can no longer contribute to a future output sample.
  const int keep_from = (int)floor(position_) - half;
  if (keep_from > 0) {
    const size_t drop = (size_t)keep_from * 2;
    buffer_.erase(buffer_.begin(), buffer_.begin() + drop);
    buffer_frames_ -= keep_from;
    position_ -= keep_from;
  }

  return written;
}
