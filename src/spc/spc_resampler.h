#ifndef SPC_RESAMPLER_H
#define SPC_RESAMPLER_H

#include <stdint.h>
#include <vector>

// Streaming stereo resampler used to convert the S-DSP's fixed 32 kHz output to
// the rate mdx2wav was asked for (44.1 kHz by default).
//
// The converter is a windowed-sinc polyphase FIR.  A 33-tap kernel is enough to
// keep the 16 kHz band of the SNES clean while avoiding the aliasing/imaging
// that linear interpolation would leave behind.  Input and output are
// interleaved signed 16-bit stereo.
class SPCResampler {
 public:
  SPCResampler();

  void Init(int in_rate, int out_rate);
  void Reset();

  // Append input frames.  The resampler keeps the last few frames internally,
  // so callers can push arbitrary chunk sizes.
  void Push(const int16_t* frames, int frame_count);

  // Write at most max_frames output frames.  Returns the number written; a
  // short return means more input is needed before Pull() can make progress.
  int Pull(int16_t* out, int max_frames);

  bool IsConfigured() const { return configured_; }
  int  InputRate() const { return in_rate_; }
  int  OutputRate() const { return out_rate_; }

 private:
  void BuildCoefficients();

  static const int kTaps = 33;    // odd, so the kernel is centred on a sample
  static const int kPhases = 512;

  bool configured_;
  int in_rate_;
  int out_rate_;
  double step_;

  std::vector<float> coeffs_;      // (kPhases + 1) * kTaps
  std::vector<float> buffer_;      // interleaved stereo, float
  int buffer_frames_;
  double position_;                // next output position, in input frames
};

#endif  // SPC_RESAMPLER_H
