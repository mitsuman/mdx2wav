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

// Decode a BRR stream for display purposes.  This mirrors the S-DSP's decoder
// closely enough to draw the waveform, but it is not used for audio.
int DecodeBRR(const uint8_t* ram, int address, int loop_address,
              int16_t* out, int max_samples, bool* looped_out,
              int* loop_samples_out) {
  int written = 0;
  int addr = address & 0xFFFF;
  const int loop_addr = loop_address & 0xFFFF;
  int prev1 = 0;
  int prev2 = 0;

  bool looped = false;
  int loop_length = 0;
  for (int block = 0; block < 256 && written < max_samples; block++) {
    const int header = ram[addr];
    addr = (addr + 1) & 0xFFFF;

    // S-DSP BRR header: bit 0 = end, bit 1 = loop.
    const bool end_flag = (header & 0x01) != 0;
    const bool loop_flag = (header & 0x02) != 0;
    if (loop_flag) looped = true;

    const int shift = header >> 4;
    const int filter = header & 0x0C;

    for (int i = 0; i < 8 && written < max_samples; i++) {
      const int byte = ram[(addr + i) & 0xFFFF];
      for (int nibble = 0; nibble < 2 && written < max_samples; nibble++) {
        int s = nibble == 0 ? (byte >> 4) : (byte & 0x0F);
        if (s >= 8) s -= 16;

        s = (s << shift) >> 1;
        if (shift >= 0xD) {
          s = (s >> 25) << 11;
        }

        const int p1 = prev1;
        const int p2 = prev2 >> 1;
        if (filter >= 8) {
          s += p1;
          s -= p2;
          if (filter == 8) {
            s += p2 >> 4;
            s += (p1 * -3) >> 6;
          } else {
            s += (p1 * -13) >> 7;
            s += (p2 * 3) >> 4;
          }
        } else if (filter != 0) {
          s += p1 >> 1;
          s += (-p1) >> 5;
        }

        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        s = (int16_t)(s * 2);
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;

        prev2 = prev1;
        prev1 = s;
        out[written++] = (int16_t)s;
      }
    }

    addr = (addr + 8) & 0xFFFF;
    if (end_flag) {
      // A looped sample usually sets both end and loop on its final block.
      // Follow the loop for the display so short loops fill the strip.
      if (loop_flag && loop_addr != 0) {
        if (loop_length == 0) loop_length = written;
        addr = loop_addr;
      } else {
        break;
      }
    }
  }

  if (looped_out) *looped_out = looped;
  if (loop_samples_out) *loop_samples_out = loop_length;
  return written;
}

int FreqToNote(double freq) {
  if (freq <= 0.0) return 0;
  double note = 69.0 + 12.0 * log(freq / 440.0) / log(2.0);
  if (note < 0.0) note = 0.0;
  if (note > 127.0) note = 127.0;
  return (int)(note + 0.5);
}

int PitchToNote(int pitch) {
  return FreqToNote(32000.0 * (double)pitch / 4096.0);
}

// Estimate the fundamental of a decoded BRR loop so the keyboard shows the
// note that is actually heard rather than the raw sample playback rate.  For
// noisy or one-shot samples the autocorrelation is weak and the pitch-register
// estimate is used instead.
int WaveformToNote(const int16_t* wave, int samples, int pitch) {
  if (!wave || samples < 16 || pitch <= 0) return PitchToNote(pitch);

  double mean = 0.0;
  for (int i = 0; i < samples; i++) mean += wave[i];
  mean /= samples;

  const int max_lag = samples / 2;
  if (max_lag < 3) return PitchToNote(pitch);

  double norms[SPCPlayer::kWaveformSamples / 2 + 1] = {0.0};
  double best = 0.0;
  for (int lag = 2; lag <= max_lag; lag++) {
    double corr = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;
    for (int i = 0; i + lag < samples; i++) {
      const double a = wave[i] - mean;
      const double b = wave[i + lag] - mean;
      corr += a * b;
      energy_a += a * a;
      energy_b += b * b;
    }
    if (energy_a > 1e-9 && energy_b > 1e-9) {
      norms[lag] = corr / sqrt(energy_a * energy_b);
      if (norms[lag] > best) best = norms[lag];
    }
  }

  if (best < 0.5) return PitchToNote(pitch);

  // Prefer the first strong *local* peak.  The global maximum can be a
  // multiple of the period, and a smooth low-frequency wave can have a high
  // correlation at very short lags; the first real peak is the fundamental.
  const double threshold = best * 0.9;
  int best_lag = 0;
  for (int lag = 3; lag < max_lag; lag++) {
    if (norms[lag] >= threshold && norms[lag] >= norms[lag - 1] &&
        norms[lag] >= norms[lag + 1]) {
      best_lag = lag;
      break;
    }
  }
  if (best_lag <= 0) return PitchToNote(pitch);

  const double sample_freq = 32000.0 / (double)best_lag;
  const double freq = sample_freq * (double)pitch / 4096.0;
  return FreqToNote(freq);
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
    s.kon = (kon & (1 << i)) != 0;
    s.pitch = pitch;
    s.note = PitchToNote(pitch);
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

    if (s.key_on) {
      int start = s.loop_address ? s.loop_address : s.source_address;
      if (!start) start = s.brr_address;
      const int loop_target = s.loop_address ? s.loop_address : start;
      s.waveform_samples = DecodeBRR(ram, start, loop_target, s.waveform,
                                     kWaveformSamples, &s.looped,
                                     &s.loop_samples);

      // Keep the pitch analysis window bounded; the display buffer itself can
      // be longer (loops are followed to fill it).
      const int analysis_samples = std::min(s.waveform_samples, 512);
      s.note = WaveformToNote(s.waveform, analysis_samples, pitch);
    } else {
      s.source_address = 0;
      s.loop_address = 0;
      s.brr_address = 0;
      s.looped = false;
      s.loop_samples = 0;
      s.waveform_samples = 0;
      s.note = 0;
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
