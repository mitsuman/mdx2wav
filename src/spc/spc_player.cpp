#include "spc_player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "../../gamdx/snes_spc/SNES_SPC.h"
#include "../../gamdx/snes_spc/SPC_Filter.h"

namespace {

const int kInputFrames = 1024;  // 32 ms at 32 kHz; a good emulation chunk
const int kDefaultSampleRate = 44100;

std::string ReadTag(const uint8_t* data, size_t size, size_t offset, size_t len) {
  if (offset >= size) return std::string();
  if (offset + len > size) len = size - offset;
  size_t end = 0;
  while (end < len && data[offset + end] != 0) end++;
  while (end > 0 && (data[offset + end - 1] == ' ' || data[offset + end - 1] == '\t')) {
    end--;
  }
  return std::string((const char*)data + offset, end);
}

// In-place iterative radix-2 FFT, used for the pitch estimate below.
void FFT(float* re, float* im, int n) {
  const double kPi = 3.14159265358979323846;
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / len;
    const double wr = cos(ang), wi = sin(ang);
    for (int i = 0; i < n; i += len) {
      double cr = 1.0, ci = 0.0;
      for (int j = 0; j < len / 2; j++) {
        const float ur = re[i + j], ui = im[i + j];
        const float xr = re[i + j + len / 2], xi = im[i + j + len / 2];
        const double vr = xr * cr - xi * ci;
        const double vi = xr * ci + xi * cr;
        re[i + j] = (float)(ur + vr);
        im[i + j] = (float)(ui + vi);
        re[i + j + len / 2] = (float)(ur - vr);
        im[i + j + len / 2] = (float)(ui - vi);
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

// Walk the BRR block chain to find the end of the sample/loop.  Only the
// header is needed, so this is much cheaper than decoding the samples.
int FindBRREnd(const uint8_t* ram, int start) {
  int addr = start & 0xFFFF;
  for (int block = 0; block < 2048; block++) {
    const int header = ram[addr];
    addr = (addr + 9) & 0xFFFF;
    if (header & 0x01) return addr;  // end flag (bit 0)
  }
  return addr;
}

// Measure the note a BRR sample plays at 32 kHz.  The SPC file does not store
// this value, so it is estimated once from the decoded sample: every candidate
// fundamental is scored by how much energy its first eight harmonics carry.
// The comb (rather than a plain FFT peak) avoids octave errors on samples whose
// second or third harmonic is louder than the fundamental.  Returns 0 for
// silence and unpitched material such as drums or noise.
float EstimateSampleTune(const int16_t* samples, int count) {
  if (!samples || count < 16) return 0.0f;
  const double kPi = 3.14159265358979323846;
  const int n = 4096;
  const int fft_n = n;

  double mean = 0.0, energy = 0.0;
  for (int i = 0; i < count; i++) {
    mean += samples[i];
    energy += (double)samples[i] * samples[i];
  }
  if (energy < 1e6) return 0.0f;  // effectively silent
  mean /= count;

  std::vector<float> re(fft_n, 0.0f), im(fft_n, 0.0f);
  for (int i = 0; i < n; i++) {
    const double w = 0.5 - 0.5 * cos(2.0 * kPi * i / (n - 1));
    re[i] = (float)((samples[i % count] - mean) * w);
  }
  FFT(re.data(), im.data(), fft_n);

  const int half = fft_n / 2;
  std::vector<float> mag(half + 1, 0.0f);
  for (int k = 0; k <= half; k++) mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);

  const double bin_hz = 32000.0 / fft_n;
  double best_score = 0.0;
  double best_freq = 0.0;
  for (double f = 30.0; f <= 3000.0; f += 0.5) {
    double score = 0.0;
    for (int h = 1; h <= 8; h++) {
      const double fh = f * h;
      if (fh > 15000.0) break;
      const int k = (int)(fh / bin_hz + 0.5);
      double m = 0.0;
      for (int d = -1; d <= 1; d++) {
        const int idx = k + d;
        if (idx >= 1 && idx <= half) m = std::max(m, (double)mag[idx]);
      }
      score += m / h;
    }
    if (score > best_score) {
      best_score = score;
      best_freq = f;
    }
  }
  if (best_freq <= 0.0) return 0.0f;

  // Reject material without a period: at a harmonic peak, a tonal sample
  // correlates strongly with itself one period later.
  const int lag = (int)(32000.0 / best_freq + 0.5);
  if (lag < 4 || lag >= n / 2) return 0.0f;
  double corr = 0.0, ea = 0.0, eb = 0.0;
  for (int i = 0; i + lag < n; i++) {
    const double a = samples[i % count] - mean;
    const double b = samples[(i + lag) % count] - mean;
    corr += a * b;
    ea += a * a;
    eb += b * b;
  }
  if (ea <= 0.0 || eb <= 0.0) return 0.0f;
  const double norm = corr / sqrt(ea * eb);
  if (norm < 0.30) return 0.0f;

  const double note = 69.0 + 12.0 * log(best_freq / 440.0) / log(2.0);
  return (float)note;
}

// Decode one BRR sample and return the note it plays at 32 kHz.  The value is
// a fixed property of the sample, so it only has to be found once per SRCN;
// voices then get their note from the pitch register alone.
float SampleTuneFromBRR(const uint8_t* ram, int dir, int srcn) {
  const int entry = (dir + srcn * 4) & 0xFFFF;
  const int start = ram[entry] | (ram[(entry + 1) & 0xFFFF] << 8);
  const int loop = ram[(entry + 2) & 0xFFFF] | (ram[(entry + 3) & 0xFFFF] << 8);
  int addr = loop ? loop : start;
  if (!addr) return 0.0f;

  std::vector<int16_t> decoded;
  decoded.reserve(4096);
  int p1 = 0, p2 = 0;
  bool ended = false;
  for (int block = 0; block < 512 && !ended; block++) {
    const int header = ram[addr];
    const int shift = header >> 4;
    const int filter = header & 0x0C;
    for (int i = 0; i < 16; i++) {
      const int byte = ram[(addr + 1 + i / 2) & 0xFFFF];
      const int nibble = (i & 1) ? (byte & 0x0F) : (byte >> 4);
      int sample = ((int8_t)(nibble << 4)) >> 4;
      sample = (sample << shift) >> 1;
      const int pp1 = p1, pp2 = p2 >> 1;
      if (filter >= 8) {
        sample += pp1 - pp2;
        if (filter == 8) {
          sample += pp2 >> 4;
          sample += (pp1 * -3) >> 6;
        } else {
          sample += (pp1 * -13) >> 7;
          sample += (pp2 * 3) >> 4;
        }
      } else if (filter) {
        sample += pp1 >> 1;
        sample += (-pp1) >> 5;
      }
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      sample *= 2;
      p2 = p1;
      p1 = sample;
      decoded.push_back((int16_t)sample);
    }
    addr = (addr + 9) & 0xFFFF;
    if (header & 0x01) ended = true;  // end flag
    if (decoded.size() >= 4096) break;
  }
  if (decoded.size() < 16) return 0.0f;
  return EstimateSampleTune(decoded.data(), (int)decoded.size());
}

}  // namespace

SPCPlayer::SPCPlayer()
    : spc_(new SNES_SPC),
      filter_(new SPC_Filter),
      loaded_(false),
      finished_(false),
      sample_rate_(kDefaultSampleRate),
      loop_(2),
      max_duration_(300.0),
      duration_(0.0),
      fadeout_seconds_(0.0),
      elapsed_(0.0),
      id666_seconds_(0.0) {
  for (int i = 0; i < kVoices; i++) key_on_seen_[i] = 0;
  spc_->init();
  raw_input_.resize(kInputFrames * 2);
}

SPCPlayer::~SPCPlayer() {
  delete filter_;
  delete spc_;
}

bool SPCPlayer::IsSPCFile(const uint8_t* data, size_t size) {
  static const char kSignature[] = "SNES-SPC700 Sound File Data";
  return data && size >= 0x10180 && memcmp(data, kSignature, sizeof(kSignature) - 1) == 0;
}

void SPCPlayer::ParseId666(const uint8_t* data, size_t size) {
  title_.clear();
  game_.clear();
  artist_.clear();
  dump_date_.clear();
  id666_seconds_ = 0.0;

  if (size < 0x10200 || data[0x23] == 0) {
    return;
  }

  title_ = ReadTag(data, size, 0x2E, 32);
  game_ = ReadTag(data, size, 0x4E, 32);
  artist_ = ReadTag(data, size, 0x6E, 32);
  dump_date_ = ReadTag(data, size, 0x8E, 11);

  // Some dumpers only fill the extended tag fields.
  if (title_.empty()) title_ = ReadTag(data, size, 0xC6, 32);
  if (game_.empty()) game_ = ReadTag(data, size, 0xE6, 32);
  if (artist_.empty()) artist_ = ReadTag(data, size, 0xA6, 32);

  // "Seconds until fadeout" is three ASCII digits at 0x9E.  Many SPC files
  // leave it blank; in that case the caller's maximum duration wins.
  if (size >= 0xA1) {
    int seconds = 0;
    bool any = false;
    for (int i = 0; i < 3; i++) {
      const uint8_t c = data[0x9E + i];
      if (c >= '0' && c <= '9') {
        seconds = seconds * 10 + (c - '0');
        any = true;
      } else {
        break;
      }
    }
    if (any && seconds > 0) {
      // The field includes the fadeout itself, so it is a good song length.
      id666_seconds_ = (double)seconds;
    }
  }
}

bool SPCPlayer::Load(const uint8_t* data, size_t size, std::string* error) {
  loaded_ = false;
  finished_ = false;
  elapsed_ = 0.0;

  if (!IsSPCFile(data, size)) {
    if (error) *error = "Not an SPC file";
    return false;
  }

  spc_data_.assign(data, data + size);

  const char* err = spc_->load_spc(spc_data_.data(), (long)spc_data_.size());
  if (err) {
    spc_data_.clear();
    if (error) *error = err;
    return false;
  }

  spc_->clear_echo();
  filter_->clear();
  resampler_.Reset();
  ClearSampleTunes();
  for (int i = 0; i < kVoices; i++) key_on_seen_[i] = 0;
  ParseId666(data, size);

  loaded_ = true;
  return true;
}

void SPCPlayer::SetSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 192000) rate = 192000;
  sample_rate_ = rate;
}

void SPCPlayer::Start(int loop, double max_duration, double fadeout_seconds) {
  loop_ = loop > 0 ? loop : 1;
  max_duration_ = max_duration;
  fadeout_seconds_ = fadeout_seconds > 0.0 ? fadeout_seconds : 0.0;
  elapsed_ = 0.0;
  finished_ = false;

  // A maximum duration of 0 means "no limit" even when the ID666 tag has a
  // length.  Otherwise the tag gives a better song length than the default,
  // and the caller's maximum still wins when it is shorter.
  if (max_duration_ <= 0.0) {
    duration_ = 0.0;
  } else {
    duration_ = id666_seconds_ > 0.0 ? id666_seconds_ : max_duration_;
    if (duration_ > max_duration_) duration_ = max_duration_;
  }

  if (loaded_ && !spc_data_.empty()) {
    // Restart from the saved APU image rather than from wherever the program
    // happened to be.  This is what makes the web player's restart and the
    // native/web PCM comparison start at the same sample.
    if (spc_->load_spc(spc_data_.data(), (long)spc_data_.size()) != 0) {
      finished_ = true;
    }
    spc_->clear_echo();
    for (int i = 0; i < kVoices; i++) key_on_seen_[i] = 0;
  }

  resampler_.Init(SNES_SPC::sample_rate, sample_rate_);
  filter_->clear();
}

bool SPCPlayer::GenerateInput() {
  const char* err = spc_->play(kInputFrames * 2, (short*)raw_input_.data());
  if (err) {
    finished_ = true;
    return false;
  }
  filter_->run((short*)raw_input_.data(), kInputFrames * 2);
  resampler_.Push(raw_input_.data(), kInputFrames);
  return true;
}

void SPCPlayer::ApplyFadeout(int16_t* out, int frames) {
  if (fadeout_seconds_ <= 0.0 || duration_ <= 0.0) return;

  const double start = duration_ - fadeout_seconds_;
  for (int i = 0; i < frames; i++) {
    const double t = elapsed_ + (double)i / sample_rate_;
    double gain = 1.0;
    if (t >= duration_) {
      gain = 0.0;
    } else if (t > start) {
      gain = (duration_ - t) / fadeout_seconds_;
    }
    if (gain < 0.0) gain = 0.0;
    if (gain < 1.0) {
      out[i * 2] = (int16_t)(out[i * 2] * gain);
      out[i * 2 + 1] = (int16_t)(out[i * 2 + 1] * gain);
    }
  }
}

int SPCPlayer::Generate(int16_t* out, int frames) {
  if (!out || frames <= 0) return 0;

  if (!loaded_ || finished_) {
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
    return frames;
  }

  int done = 0;
  while (done < frames) {
    const int got = resampler_.Pull(out + done * 2, frames - done);
    done += got;
    if (done >= frames) break;
    if (!GenerateInput()) break;
  }

  if (done < frames) {
    memset(out + done * 2, 0, (size_t)(frames - done) * 2 * sizeof(int16_t));
    finished_ = true;
  }

  ApplyFadeout(out, frames);
  elapsed_ += (double)frames / sample_rate_;

  if (duration_ > 0.0 && elapsed_ >= duration_) {
    finished_ = true;
  }
  return frames;
}

void SPCPlayer::ClearSampleTunes() {
  sample_tunes_.clear();
}

bool SPCPlayer::GetSampleTune(const uint8_t* ram, int dir, int srcn,
                              float* tune) const {
  if (!tune || !ram) return false;
  auto it = sample_tunes_.find(srcn);
  if (it != sample_tunes_.end()) {
    if (it->second < 0.0f) return false;  // known to be unpitched
    *tune = it->second;
    return true;
  }
  const float note = SampleTuneFromBRR(ram, dir, srcn);
  sample_tunes_[srcn] = note > 0.0f ? note : -1.0f;
  if (note <= 0.0f) return false;
  *tune = note;
  return true;
}

void SPCPlayer::GetVoiceStates(VoiceState out[kVoices]) const {
  if (!out) return;
  memset(out, 0, sizeof(VoiceState) * kVoices);
  if (!loaded_) return;

  SPC_DSP& dsp = spc_->dsp_state();
  const SPC_DSP::voice_t* voices = dsp.voices();
  const uint8_t* ram = spc_->smp_ram();
  const int dir = (dsp.read(SPC_DSP::r_dir) << 8) & 0xFFFF;
  const int kon = dsp.read(SPC_DSP::r_kon);

  for (int i = 0; i < kVoices; i++) {
    VoiceState& s = out[i];
    const int base = i * 0x10;
    const int pitch = dsp.read(base + SPC_DSP::v_pitchl) |
                      (dsp.read(base + SPC_DSP::v_pitchh) << 8);
    const int voll = (int8_t)dsp.read(base + SPC_DSP::v_voll);
    const int volr = (int8_t)dsp.read(base + SPC_DSP::v_volr);
    const int srcn = dsp.read(base + SPC_DSP::v_srcn);
    const int env = voices[i].env;
    const int max_vol = std::max(std::abs(voll), std::abs(volr));

    s.key_on = env > 0;
    const unsigned long key_ons = dsp.dbg_key_on(i);
    s.note_on = key_ons != key_on_seen_[i];
    if (s.note_on) key_on_seen_[i] = key_ons;
    s.kon = (kon & (1 << i)) != 0;
    s.pitch = pitch;
    s.note = 0;  // filled in below from the pitch register + sample tuning
    s.volume = std::min(127, (env * max_vol) / 2047);
    s.left_volume = voll;
    s.right_volume = volr;
    s.envelope = env;
    switch (voices[i].env_mode) {
      case SPC_DSP::env_release: s.envelope_phase = 0; break;
      case SPC_DSP::env_attack: s.envelope_phase = 1; break;
      case SPC_DSP::env_decay: s.envelope_phase = 2; break;
      case SPC_DSP::env_sustain: s.envelope_phase = 3; break;
      default: s.envelope_phase = 0; break;
    }
    s.sample_rate = (int)(32000.0 * (double)pitch / 4096.0);
    s.source_number = srcn;

    const int entry = (dir + srcn * 4) & 0xFFFF;
    s.source_address = ram[entry] | (ram[(entry + 1) & 0xFFFF] << 8);
    s.loop_address = ram[(entry + 2) & 0xFFFF] | (ram[(entry + 3) & 0xFFFF] << 8);
    s.brr_address = voices[i].brr_addr;
    if (s.source_address || s.loop_address || s.brr_address) {
      const int start = s.source_address ? s.source_address
                       : (s.loop_address ? s.loop_address : s.brr_address);
      s.sample_end_address = FindBRREnd(ram, start);
    }

    if (s.key_on) {
      // Copy the DSP's actual per-voice output ring.  This already contains
      // envelope, volume, pitch, BRR interpolation and noise, so no synthetic
      // re-decoding is needed.
      const int ring_size = SPC_DSP::dbg_wave_size;
      const int16_t* ring = dsp.dbg_waveform(i);
      const int pos = dsp.dbg_waveform_pos(i);
      const int count = std::min(kWaveformSamples, ring_size);
      for (int j = 0; j < count; j++) {
        s.waveform[j] = ring[(pos - count + j + ring_size) & (ring_size - 1)];
      }
      s.waveform_samples = count;

      // The note follows the pitch register.  Only the sample's own tuning is
      // measured (once, from its BRR data) so that the register value can be
      // turned into an absolute note; pitch slides then track exactly.
      float tune = 0.0f;
      if (pitch > 0 && GetSampleTune(ram, dir, srcn, &tune)) {
        float n = tune + 12.0f * (float)log2((double)pitch / 4096.0);
        if (n < 0.0f) n = 0.0f;
        if (n > 127.0f) n = 127.0f;
        s.pitch_note = n;
        s.note = (int)(n + 0.5f);
      }
    } else {
      s.source_address = 0;
      s.loop_address = 0;
      s.brr_address = 0;
      s.sample_end_address = 0;
      s.waveform_samples = 0;
      s.note = 0;
      s.pitch_note = 0.0f;
    }
  }
}

void SPCPlayer::GetGlobalState(GlobalState* out) const {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!loaded_) return;

  SPC_DSP& dsp = spc_->dsp_state();
  out->master_volume_left = (int8_t)dsp.read(SPC_DSP::r_mvoll);
  out->master_volume_right = (int8_t)dsp.read(SPC_DSP::r_mvolr);
  out->echo_volume_left = (int8_t)dsp.read(SPC_DSP::r_evoll);
  out->echo_volume_right = (int8_t)dsp.read(SPC_DSP::r_evolr);
  out->echo_feedback = (int8_t)dsp.read(SPC_DSP::r_efb);
  out->echo_delay = dsp.read(SPC_DSP::r_edl) & 0x0F;
  out->echo_start = dsp.read(SPC_DSP::r_esa);
  out->echo_offset = dsp.echo_offset();
  out->echo_length = dsp.echo_length();
  out->noise_enable = dsp.read(SPC_DSP::r_flg) & 0x20;
  out->key_on_mask = dsp.read(SPC_DSP::r_kon);
  out->directory_address = dsp.read(SPC_DSP::r_dir);
  out->cpu_pc = spc_->cpu_pc();
}
