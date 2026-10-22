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

// Estimate the note of the actual per-voice output.  The captured samples are
// already at the final playback rate, so the autocorrelation period maps
// directly to frequency.  Returns 0 for noise/unpitched voices.
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

int OutputWaveformToNote(const int16_t* wave, int samples) {
  if (!wave || samples < 16) return 0;

  double mean = 0.0;
  for (int i = 0; i < samples; i++) mean += wave[i];
  mean /= samples;

  const int max_lag = samples / 2;
  if (max_lag < 8) return 0;
  double norms[513] = {0.0};
  if (max_lag > 512) return 0;

  // Start at lag 8: near-DC and very high harmonics otherwise dominate the
  // autocorrelation of bass notes.  This still covers notes down to ~250 Hz
  // comfortably, and the longer 1024-sample window reaches the low bass.
  double best = 0.0;
  for (int lag = 8; lag <= max_lag; lag++) {
    double corr = 0.0, ea = 0.0, eb = 0.0;
    for (int i = 0; i + lag < samples; i++) {
      const double a = wave[i] - mean;
      const double b = wave[i + lag] - mean;
      corr += a * b;
      ea += a * a;
      eb += b * b;
    }
    if (ea > 1e-9 && eb > 1e-9) {
      norms[lag] = corr / sqrt(ea * eb);
      if (norms[lag] > best) best = norms[lag];
    }
  }

  if (best < 0.5) return 0;
  const double threshold = best * 0.85;
  int best_lag = 0;
  for (int lag = 9; lag < max_lag; lag++) {
    if (norms[lag] >= threshold && norms[lag] >= norms[lag - 1] &&
        norms[lag] >= norms[lag + 1]) {
      best_lag = lag;
      break;
    }
  }
  if (best_lag <= 0) return 0;
  return FreqToNote(32000.0 / (double)best_lag);
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
      s.note = OutputWaveformToNote(s.waveform, count);
    } else {
      s.source_address = 0;
      s.loop_address = 0;
      s.brr_address = 0;
      s.sample_end_address = 0;
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
