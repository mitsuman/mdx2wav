#include "zmd_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../gamdx/fmgen/opm.h"
#include "../../gamdx/mxdrvg/opm_delegate.h"
#include "msm6258.h"

namespace {

// YM2151 key code table from Z-MUSIC (unified/interrupt.s).
const uint8_t kKcTable[128] = {
    0x0C, 0x0D, 0x0E, 0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A,
    0x0C, 0x0D, 0x0E, 0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A,
    0x0C, 0x0D, 0x0E, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x18, 0x19, 0x1A,
    0x1C, 0x1D, 0x1E, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A,
    0x2C, 0x2D, 0x2E, 0x30, 0x31, 0x32, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3A,
    0x3C, 0x3D, 0x3E, 0x40, 0x41, 0x42, 0x44, 0x45, 0x46, 0x48, 0x49, 0x4A,
    0x4C, 0x4D, 0x4E, 0x50, 0x51, 0x52, 0x54, 0x55, 0x56, 0x58, 0x59, 0x5A,
    0x5C, 0x5D, 0x5E, 0x60, 0x61, 0x62, 0x64, 0x65, 0x66, 0x68, 0x69, 0x6A,
    0x6C, 0x6D, 0x6E, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A,
    0x7C, 0x7D, 0x7E, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A,
    0x7C, 0x7D, 0x7E, 0x70, 0x71, 0x72, 0x74, 0x75,
};

// Register offsets for OP1, OP2, OP3, OP4 (note the 0x10/0x08 swap).
const int kOpOffset[4] = {0x00, 0x10, 0x08, 0x18};
// Voice data base offsets for OP1..OP4.
const int kOpData[4] = {0x0b, 0x16, 0x21, 0x2c};
// Carrier table from Z-MUSIC.
const uint8_t kCarrier[8] = {0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F};

inline int Clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

ZmdPlayer::ZmdPlayer()
    : adpcm_(512),
      adpcm_freq_(4),
      adpcm_pan_(3),
      adpcm_track_volume_(0),
      adpcm_ready_(false),
      has_adpcm_(false),
      sample_rate_(44100),
      loop_limit_(2),
      tempo_(120),
      mclk_(192),
      started_(false),
      all_dead_(false),
      tail_started_(false),
      tail_remaining_(0),
      paused_(false),
      tick_accum_(0.0),
      samples_per_tick_(0.0),
      total_seconds_(0.0),
      adpcm_keyons_(0),
      register_sink_(nullptr),
      opm_(nullptr) {}

ZmdPlayer::~ZmdPlayer() {
  delete opm_;
  opm_ = nullptr;
}

void ZmdPlayer::SetSampleRate(int sample_rate) {
  sample_rate_ = sample_rate > 0 ? sample_rate : 44100;
  if (started_) samples_per_tick_ = sample_rate_ * TickSeconds();
}

void ZmdPlayer::SetLoopCount(int loops) { loop_limit_ = loops; }

void ZmdPlayer::WriteOpmDirect(uint8_t addr, uint8_t data) {
  if (opm_) opm_->SetReg(addr, data);
}

void ZmdPlayer::WriteReg(uint8_t addr, uint8_t data) {
  if (!opm_) return;
  if (register_sink_) {
    register_sink_->SetReg(addr, data);
  } else {
    opm_->SetReg(addr, data);
  }
}

int ZmdPlayer::UsedFmChannels() const {
  int used = 0;
  for (size_t i = 0; i < tracks_.size(); i++) {
    const int ch = tracks_[i].channel;
    if (ch >= 0 && ch < 8 && ch + 1 > used) used = ch + 1;
  }
  return used;
}

bool ZmdPlayer::GetAdpcmVisualState(AdpcmVisualState* out) const {
  if (!out) return false;
  *out = AdpcmVisualState();
  const int note = adpcm_play_.note;
  if (note < 0 || note >= (int)adpcm_.size() || !adpcm_[note].valid) {
    return false;
  }
  const AdpcmSample& sample = adpcm_[note];
  static const double kRates[5] = {3900.0, 5200.0, 7800.0, 10400.0, 15600.0};
  const int freq = Clamp(adpcm_freq_, 0, 4);
  out->sample_valid = true;
  out->active = adpcm_play_.active;
  out->note = note;
  out->rate = (int)llround(kRates[freq] * pow(2.0, (sample.pitch - 12) / 12.0));
  out->volume = sample.volume;
  out->pan = adpcm_pan_;
  out->pcm = sample.pcm.empty() ? nullptr : sample.pcm.data();
  out->pcm_frames = (int)sample.pcm.size();
  out->position = adpcm_play_.pos;
  out->step = adpcm_play_.step;
  out->keyons = adpcm_keyons_;
  return true;
}

int ZmdPlayer::GetAdpcmWaveform(int16_t* out, int max_samples) const {
  if (!out || max_samples <= 0) return 0;
  const int note = adpcm_play_.note;
  if (!adpcm_play_.active || note < 0 || note >= (int)adpcm_.size() ||
      !adpcm_[note].valid) {
    return 0;
  }
  const std::vector<int16_t>& pcm = adpcm_[note].pcm;
  if (pcm.size() < 2) return 0;
  double step = adpcm_play_.step;
  if (!(step > 0.0)) step = 1.0;
  double pos = adpcm_play_.pos - (double)max_samples * step;
  if (pos < 0.0) pos = 0.0;
  for (int i = 0; i < max_samples; i++) {
    if (pos >= (double)(pcm.size() - 1)) {
      out[i] = 0;
      continue;
    }
    const size_t idx = (size_t)pos;
    const double frac = pos - (double)idx;
    out[i] = (int16_t)llround((double)pcm[idx] * (1.0 - frac) +
                              (double)pcm[idx + 1] * frac);
    pos += step;
  }
  return max_samples;
}

uint16_t ZmdPlayer::Be16(size_t pos) const {
  return (uint16_t)((data_[pos] << 8) | data_[pos + 1]);
}

uint32_t ZmdPlayer::Be32(size_t pos) const {
  return ((uint32_t)data_[pos] << 24) | ((uint32_t)data_[pos + 1] << 16) |
         ((uint32_t)data_[pos + 2] << 8) | (uint32_t)data_[pos + 3];
}

bool ZmdPlayer::LoadFile(const char* path, std::string* error) {
  const std::string p(path);
  const size_t slash = p.find_last_of('/');
  base_dir_ = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
  FILE* f = fopen(path, "rb");
  if (!f) {
    if (error) *error = std::string("Cannot open ") + path;
    return false;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    if (error) *error = "Cannot seek";
    return false;
  }
  long size = ftell(f);
  if (size <= 0 || size > 64 * 1024 * 1024) {
    fclose(f);
    if (error) *error = "Invalid file size";
    return false;
  }
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)size);
  size_t got = fread(buf.data(), 1, buf.size(), f);
  fclose(f);
  if (got != buf.size()) {
    if (error) *error = "Short read";
    return false;
  }
  return Load(buf.data(), buf.size(), error);
}

bool ZmdPlayer::Load(const uint8_t* data, size_t size, std::string* error) {
  data_.assign(data, data + size);
  error_.clear();
  voices_.assign(200, Voice());
  tracks_.clear();
  adpcm_.assign(512, AdpcmSample());
  adpcm_play_ = AdpcmPlay();
  adpcm_freq_ = 4;
  adpcm_pan_ = 3;
  adpcm_track_volume_ = 0;
  adpcm_ready_ = false;
  adpcm_warning_.clear();
  title_.clear();
  has_adpcm_ = false;
  tempo_ = 120;
  mclk_ = 192;
  loop_limit_ = 2;

  if (!Parse(error)) {
    if (error) *error = error_;
    return false;
  }
  started_ = false;
  return true;
}

bool ZmdPlayer::Parse(std::string* error) {
  const uint8_t* d = data_.data();
  const size_t n = data_.size();

  auto fail = [&](const char* msg, size_t pos) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s at 0x%zx", msg, pos);
    error_ = buf;
    if (error) *error = error_;
    return false;
  };

  if (n < 8 || d[0] != 0x10 || memcmp(d + 1, "ZmuSiC", 6) != 0) {
    return fail("Not a ZMD file", 0);
  }

  size_t pos = 8;
  bool ended = false;
  while (pos < n) {
    const uint8_t cmd = d[pos++];
    if (cmd == 0xff) {
      if (pos & 1) pos++;
      ended = true;
      break;
    }
    switch (cmd) {
      case 0x04:
      case 0x1b: {
        if (pos + 56 > n) return fail("Truncated voice", pos);
        const int num = d[pos++];
        if (num < 1 || num > 200) return fail("Bad voice number", pos - 1);
        memcpy(voices_[num - 1].data, d + pos, 55);
        pos += 55;
        break;
      }
      case 0x05: {
        if (pos + 2 > n) return fail("Truncated tempo", pos);
        tempo_ = Be16(pos);
        pos += 2;
        break;
      }
      case 0x15:
        if (pos + 1 > n) return fail("Truncated base channel", pos);
        pos += 1;
        break;
      case 0x18: {
        if (pos + 2 > n) return fail("Truncated MIDI data", pos);
        const int len = Be16(pos);
        pos += 2;
        if (pos + len > n) return fail("Truncated MIDI data", pos);
        pos += len;
        break;
      }
      case 0x40: {
        // ADPCM configuration: destination note, pitch, volume, mix delay,
        // mix note, cut offset/size, reverse, fade, then a filename (or a
        // zero word + an already-registered source note).
        if (pos + 19 > n) return fail("Truncated ADPCM config", pos);
        const int dest_note = Be16(pos);
        const int pitch = Be16(pos + 2);
        const int volume = Be16(pos + 4);
        const int cut_offset = Be16(pos + 10);
        const int cut_size = Be16(pos + 12);
        const bool reverse = d[pos + 14] != 0;
        pos += 19;
        has_adpcm_ = true;

        if (pos < n && d[pos] != 0) {
          std::string filename;
          while (pos < n && d[pos] != 0) filename.push_back((char)d[pos++]);
          pos++;
          if (dest_note >= 0 && dest_note < 512) {
            LoadZpd(filename, dest_note, pitch, volume, cut_offset, cut_size,
                    reverse);
          }
        } else {
          if (pos + 4 > n) return fail("Truncated ADPCM reference", pos);
          const int source_note = Be16(pos + 2);
          pos += 4;
          if (dest_note >= 0 && dest_note < 512 && source_note >= 0 &&
              source_note < 512 && adpcm_[source_note].valid) {
            adpcm_[dest_note] = adpcm_[source_note];
            adpcm_[dest_note].pitch = Clamp(pitch, 0, 24);
            adpcm_[dest_note].volume = Clamp(volume, 1, 300);
            adpcm_ready_ = true;
          }
        }
        break;
      }
      case 0x42: {
        if (pos + 5 > n) return fail("Truncated clock", pos);
        mclk_ = d[pos];
        pos += 5;
        break;
      }
      case 0x4a: {
        if (pos + 2 > n) return fail("Truncated wave", pos);
        const int len = Be16(pos);
        pos += 2;
        // Wave number, loop type, loop point, then `len` words of data.
        if (pos + 4 + 2 * len > n) return fail("Truncated wave", pos);
        pos += 4 + 2 * len;
        break;
      }
      case 0x60:
      case 0x61:
      case 0x62:
      case 0x63:
      case 0x7f: {
        const size_t start = pos;
        while (pos < n && d[pos] != 0) pos++;
        if (pos >= n) return fail("Unterminated string", start);
        if (cmd == 0x7f && title_.empty()) {
          title_.assign((const char*)d + start, pos - start);
        }
        pos++;
        break;
      }
      case 0x7e:
        break;
      default: {
        char msg[80];
        snprintf(msg, sizeof(msg), "Unsupported common command 0x%02x", cmd);
        return fail(msg, pos - 1);
      }
    }
  }
  if (!ended) return fail("No common command terminator", pos);

  if (pos + 2 > n) return fail("No track table", pos);
  const int track_count = Be16(pos);
  pos += 2;
  if (track_count < 1 || track_count > 64) return fail("Bad track count", pos - 2);
  if (pos + (size_t)track_count * 6 > n) return fail("Truncated track table", pos);

  for (int i = 0; i < track_count; i++) {
    const size_t entry = pos;
    const uint32_t offset = Be32(pos);
    pos += 4;
    pos += 1;  // reserved
    const int ch = d[pos++];
    const size_t start = entry + 4 + offset;
    if (start >= n) return fail("Track offset out of range", entry);
    if (ch < 0 || ch > 8) {
      // MIDI tracks (9+) are outside this port.
      continue;
    }
    Track t;
    t.channel = ch;
    t.start = start;
    t.pc = start;
    tracks_.push_back(t);
  }

  if (tracks_.empty()) return fail("No FM tracks", pos);
  if (tempo_ < 20) tempo_ = 20;
  if (tempo_ > 5000) tempo_ = 5000;
  if (mclk_ <= 0) mclk_ = 192;
  return true;
}


namespace {

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  const long size = ftell(f);
  if (size <= 0 || size > 32 * 1024 * 1024) {
    fclose(f);
    return false;
  }
  fseek(f, 0, SEEK_SET);
  out->resize((size_t)size);
  const size_t got = fread(out->data(), 1, out->size(), f);
  fclose(f);
  return got == out->size();
}

std::string WithExtension(const std::string& path, const char* ext) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    return path;
  }
  return path + ext;
}

}  // namespace

bool ZmdPlayer::LoadZpd(const std::string& filename, int dest_note, int pitch,
                        int volume, int cut_offset, int cut_size, bool reverse) {
  std::vector<std::string> candidates;
  if (!filename.empty() && filename[0] == '/') {
    candidates.push_back(filename);
  } else {
    candidates.push_back(base_dir_ + "/" + filename);
    candidates.push_back(filename);
  }
  const size_t base_count = candidates.size();
  for (size_t i = 0; i < base_count; i++) {
    candidates.push_back(WithExtension(candidates[i], ".zpd"));
    candidates.push_back(WithExtension(candidates[i], ".ZPD"));
    std::string p = candidates[i];
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".zpd") == 0) {
      candidates.push_back(p.substr(0, p.size() - 4) + ".ZPD");
    } else if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ZPD") == 0) {
      candidates.push_back(p.substr(0, p.size() - 4) + ".zpd");
    }
  }

  std::vector<uint8_t> buf;
  std::string used;
  for (size_t i = 0; i < candidates.size(); i++) {
    if (ReadWholeFile(candidates[i], &buf)) {
      used = candidates[i];
      break;
    }
  }
  if (used.empty()) {
    if (adpcm_warning_.empty()) {
      adpcm_warning_ = "ADPCM file not found: " + filename;
    }
    return false;
  }
  if (buf.size() < 8 || buf[0] != 0x10 ||
      memcmp(buf.data() + 1, "ZmAdpCm", 7) != 0) {
    if (adpcm_warning_.empty()) {
      adpcm_warning_ = "Not a ZPD file: " + used;
    }
    return false;
  }

  const int pitch_v = Clamp(pitch, 0, 24);
  const int volume_v = Clamp(volume, 1, 300);
  const size_t size = buf.size();
  size_t pos = 8;
  bool have_first = false;
  AdpcmSample first;

  while (pos + 2 <= size) {
    const int note = (buf[pos] << 8) | buf[pos + 1];
    pos += 2;
    if (note == 0xffff) break;
    if (pos + 8 > size) break;
    const size_t off_field = pos;
    const uint32_t off = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                         ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
    pos += 4;
    const uint32_t len = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                         ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
    pos += 4;
    const size_t start = off_field + 4 + off;
    if (len == 0 || start >= size || start + len > size) continue;

    std::vector<int16_t> pcm;
    zmd::DecodeMsm6258(buf.data() + start, len, &pcm);
    if (pcm.empty()) continue;

    // Cut offset/size are in ADPCM bytes; one ADPCM byte becomes two PCM
    // samples.
    int first_sample = 0;
    int sample_count = (int)pcm.size();
    if (cut_offset > 0 || cut_size > 0) {
      first_sample = Clamp(cut_offset * 2, 0, (int)pcm.size());
      sample_count = cut_size > 0 ? cut_size * 2 : (int)pcm.size() - first_sample;
      sample_count = Clamp(sample_count, 0, (int)pcm.size() - first_sample);
    }
    std::vector<int16_t> sliced(pcm.begin() + first_sample,
                                pcm.begin() + first_sample + sample_count);
    if (reverse) std::reverse(sliced.begin(), sliced.end());
    if (sliced.empty()) continue;

    AdpcmSample sample;
    sample.pcm.swap(sliced);
    sample.pitch = pitch_v;
    sample.volume = volume_v;
    sample.valid = true;

    if (note >= 0 && note < 512) adpcm_[note] = sample;
    if (!have_first) {
      first = sample;
      have_first = true;
    }
  }

  if (!have_first) {
    if (adpcm_warning_.empty()) {
      adpcm_warning_ = "No ADPCM samples in ZPD: " + used;
    }
    return false;
  }
  if (dest_note >= 0 && dest_note < 512) adpcm_[dest_note] = first;
  adpcm_ready_ = true;
  return true;
}

void ZmdPlayer::StartAdpcm(int note) {
  if (note < 0 || note >= (int)adpcm_.size() || !adpcm_[note].valid) {
    StopAdpcm();
    return;
  }
  static const double kRates[5] = {3900.0, 5200.0, 7800.0, 10400.0, 15600.0};
  const AdpcmSample& sample = adpcm_[note];
  const int freq = Clamp(adpcm_freq_, 0, 4);
  const double pitch_factor = pow(2.0, (sample.pitch - 12) / 12.0);
  adpcm_play_.active = true;
  adpcm_play_.note = note;
  adpcm_play_.pos = 0.0;
  adpcm_keyons_++;
  // The ZPD sample is stored at 15.6 kHz; resample it to the output rate.
  adpcm_play_.step = (kRates[freq] / (double)sample_rate_) * pitch_factor;
}

void ZmdPlayer::StopAdpcm() { adpcm_play_.active = false; }

void ZmdPlayer::MixAdpcm(int16_t* out, int frames) {
  if (!adpcm_play_.active) return;
  const int note = adpcm_play_.note;
  if (note < 0 || note >= (int)adpcm_.size() || !adpcm_[note].valid) {
    adpcm_play_.active = false;
    return;
  }
  const AdpcmSample& sample = adpcm_[note];
  const std::vector<int16_t>& pcm = sample.pcm;
  if (pcm.size() < 2 || adpcm_play_.step <= 0.0) {
    adpcm_play_.active = false;
    return;
  }
  const int pan = adpcm_pan_;
  const double gain = (sample.volume / 100.0) *
                      ((127 - Clamp(adpcm_track_volume_, 0, 127)) / 127.0);
  for (int i = 0; i < frames; i++) {
    if (adpcm_play_.pos >= (double)(pcm.size() - 1)) {
      adpcm_play_.active = false;
      break;
    }
    const size_t idx = (size_t)adpcm_play_.pos;
    const double frac = adpcm_play_.pos - (double)idx;
    const double v = ((double)pcm[idx] * (1.0 - frac) +
                      (double)pcm[idx + 1] * frac) * gain;
    const int iv = (int)llround(v);
    if (pan == 1 || pan == 3) {
      int l = out[i * 2] + iv;
      out[i * 2] = (int16_t)Clamp(l, -32768, 32767);
    }
    if (pan == 2 || pan == 3) {
      int r = out[i * 2 + 1] + iv;
      out[i * 2 + 1] = (int16_t)Clamp(r, -32768, 32767);
    }
    adpcm_play_.pos += adpcm_play_.step;
  }
}

double ZmdPlayer::TickSeconds() const {
  int bpm = tempo_;
  if (bpm < 20) bpm = 20;
  if (bpm > 5000) bpm = 5000;
  // OPM timer A: 16us * floor(78125 / tempo).  78125 = 16*4000*60000/(1024*48).
  double ticks = floor(78125.0 / (double)bpm);
  if (ticks < 1.0) ticks = 1.0;
  return ticks * 16.0e-6;
}

void ZmdPlayer::ResetTracks() {
  for (size_t i = 0; i < tracks_.size(); i++) {
    Track& t = tracks_[i];
    t.pc = t.start;
    t.dead = false;
    t.step_wait = 0;
    t.gate_wait = 0;
    t.note = -1;
    t.key_on = false;
    t.volume = 0x15;
    t.pan = 3;
    t.pgm = 0;
    t.detune_f = 0;
    t.detune_m = 0;
    t.has_do = false;
    t.do_ptr = 0;
    t.loop_pass = 0;
    t.repeats.clear();
    t.has_voice = false;
    t.carrier = 0x08;
    t.af = 0;
    t.om = 0x78;
    for (int k = 0; k < 4; k++) t.ol[k] = 0;
    t.pmod = Lfo();
    t.amod = Lfo();
    t.amod_offset = 0;
  }
}

void ZmdPlayer::Start() {
  if (!opm_) opm_ = new FM::OPM();
  opm_->Init(4000000, (uint32_t)sample_rate_, false);
  opm_->Reset();
  WriteReg(0x14, 0x00);  // timers off

  ResetTracks();
  tick_accum_ = 0.0;
  samples_per_tick_ = sample_rate_ * TickSeconds();
  total_seconds_ = 0.0;
  all_dead_ = false;
  tail_started_ = false;
  tail_remaining_ = 0;
  paused_ = false;
  started_ = true;

  adpcm_play_ = AdpcmPlay();
  adpcm_keyons_ = 0;
  adpcm_freq_ = 4;
  adpcm_pan_ = 3;
  adpcm_track_volume_ = 0x15;  // Z-MUSIC default p_vol
  for (size_t i = 0; i < tracks_.size(); i++) {
    Track& t = tracks_[i];
    if (t.channel < 8) {
      WriteVoice(t, 0, true);
      ApplyPan(t);
      ApplyVolume(t);
    }
  }
  // Execute the initial commands (volume, tempo, detune, rests, ...).
  for (size_t i = 0; i < tracks_.size(); i++) {
    if (!tracks_[i].dead) RunCommands(tracks_[i]);
  }
}

void ZmdPlayer::WriteVoice(Track& t, int pgm, bool preserve_pan) {
  if (t.channel >= 8) return;
  if (pgm < 0 || pgm >= (int)voices_.size()) return;
  const uint8_t* v = voices_[pgm].data;
  t.pgm = pgm;
  memcpy(t.vdata, v, 55);
  t.has_voice = true;
  t.af = v[0] & 0x3f;
  t.om = (uint8_t)((v[1] << 3) & 0x78);
  t.carrier = kCarrier[v[0] & 7];
  if (!preserve_pan) t.pan = v[9] & 3;
  for (int op = 0; op < 4; op++) {
    t.ol[op] = v[kOpData[op] + 5];  // TL
  }

  const int ch = t.channel;
  WriteReg(0x20 + ch, (uint8_t)(((t.pan & 3) << 6) | t.af));
  WriteReg(0x38 + ch, (uint8_t)(((v[7] & 0x0f) << 4) | (v[8] & 0x0f)));
  WriteReg(0x18, v[4]);
  WriteReg(0x19, (uint8_t)(v[5] | 0x80));
  WriteReg(0x1a, (uint8_t)(v[6] & 0x7f));
  WriteReg(0x1b, (uint8_t)(v[2] & 0x03));

  for (int op = 0; op < 4; op++) {
    const int base = kOpData[op];
    const int reg = 0x40 + kOpOffset[op] + ch;
    WriteReg(reg, (uint8_t)(((v[base + 8] & 0x07) << 4) | (v[base + 7] & 0x0f)));
    WriteReg(reg + 0x20, (uint8_t)(v[base + 5] & 0x7f));  // TL
    WriteReg(reg + 0x40, (uint8_t)(((v[base + 6] & 0x03) << 6) | (v[base + 0] & 0x1f)));
    WriteReg(reg + 0x60, (uint8_t)(((v[base + 10] & 0x01) << 7) | (v[base + 1] & 0x1f)));
    WriteReg(reg + 0x80, (uint8_t)(((v[base + 9] & 0x03) << 6) | (v[base + 2] & 0x1f)));
    WriteReg(reg + 0xa0, (uint8_t)(((v[base + 4] & 0x0f) << 4) | (v[base + 3] & 0x0f)));
  }
  ApplyVolume(t);
}

void ZmdPlayer::ApplyPan(Track& t) {
  if (!opm_ || t.channel >= 8) return;
  WriteReg(0x20 + t.channel, (uint8_t)(((t.pan & 3) << 6) | t.af));
}

void ZmdPlayer::ApplyVolume(Track& t) {
  if (!opm_ || t.channel >= 8) return;
  const int am = t.amod.on ? t.amod_offset : 0;
  for (int op = 0; op < 4; op++) {
    int tl = t.ol[op];
    if (t.carrier & (1 << op)) {
      tl += t.volume;
      tl -= am;
    }
    tl = Clamp(tl, 0, 127);
    WriteReg(0x60 + kOpOffset[op] + t.channel, (uint8_t)tl);
  }
}

void ZmdPlayer::SetPitch(Track& t, int note, int kf_offset) {
  if (!opm_ || t.channel >= 8) return;
  int n = note & 0xff;
  int d1 = t.detune_f + 5 + kf_offset;
  const int oct = d1 >> 6;  // arithmetic shift, same as the 68000
  if (oct != 0) {
    n += oct;
    d1 &= 0x3f;
  }
  const int kf = (d1 & 0xff) << 2;
  WriteReg(0x30 + t.channel, (uint8_t)(kf & 0xff));
  const int idx = Clamp(n, 0, 127);
  WriteReg(0x28 + t.channel, kKcTable[idx]);
}

void ZmdPlayer::KeyOn(Track& t, int note) {
  if (t.channel >= 8) {
    t.note = note;
    t.key_on = true;
    StartAdpcm(note);
    return;
  }
  if (!opm_) return;
  t.note = note;
  t.key_on = true;
  SetPitch(t, note, 0);
  WriteReg(0x08, (uint8_t)(t.channel | t.om));
  if (t.has_voice && t.vdata[3] != 0) {
    WriteReg(0x01, 0x02);
    WriteReg(0x01, 0x00);
  }
}

void ZmdPlayer::KeyOff(Track& t) {
  if (t.channel >= 8) {
    StopAdpcm();
    t.key_on = false;
    return;
  }
  if (!opm_) return;
  if (t.key_on) {
    WriteReg(0x08, (uint8_t)t.channel);
    t.key_on = false;
  }
}

void ZmdPlayer::EndTrack(Track& t) {
  if (t.dead) return;
  KeyOff(t);
  t.dead = true;
}

bool ZmdPlayer::AllDead() const {
  for (size_t i = 0; i < tracks_.size(); i++) {
    if (!tracks_[i].dead) return false;
  }
  return true;
}

void ZmdPlayer::HandleNote(Track& t, int note, int step, int gate) {
  if (t.channel >= 8) {
    t.step_wait = step > 0 ? step : 1;
    t.gate_wait = gate > 0 ? gate : 0;
    t.note = note;
    t.key_on = true;
    StartAdpcm(note);
    return;
  }
  const bool tie = t.gate_wait > 0;
  t.step_wait = step > 0 ? step : 1;
  t.gate_wait = gate > 0 ? gate : 0;

  if (!(tie && t.key_on && note == t.note)) {
    KeyOn(t, note);
  }
  ResetLfo(t, t.pmod);
  ResetLfo(t, t.amod);
}

void ZmdPlayer::HandleRest(Track& t, int step, int gate) {
  t.step_wait = step > 0 ? step : 1;
  t.gate_wait = gate > 0 ? gate : 0;
}

void ZmdPlayer::HandleTempo(int bpm) {
  tempo_ = Clamp(bpm, 20, 5000);
  samples_per_tick_ = sample_rate_ * TickSeconds();
}

void ZmdPlayer::ResetLfo(Track& t, Lfo& lfo) {
  (void)t;
  lfo.wait = lfo.delay;
  lfo.phase = 0.0;
  lfo.last_value = 0;
  if (lfo.wave == 1) {
    lfo.inc = 1.0 / (2.0 * std::max(1, lfo.speed));
  } else if (lfo.wave == 2) {
    lfo.inc = 1.0 / (4.0 * std::max(1, lfo.speed / 2));
  } else {
    lfo.inc = 1.0 / (2.0 * std::max(1, lfo.speed));
  }
}

void ZmdPlayer::RecalcPitchLfo(Track& t) {
  Lfo& l = t.pmod;
  if (l.depth == 0) {
    l.on = false;
    return;
  }
  l.on = true;
  l.inc = (l.wave == 1) ? 1.0 / (2.0 * std::max(1, l.speed))
                        : 1.0 / (4.0 * std::max(1, l.wave == 2 ? l.speed / 2 : l.speed));
}

void ZmdPlayer::RecalcAmpLfo(Track& t) {
  Lfo& l = t.amod;
  if (l.depth == 0) {
    l.on = false;
    t.amod_offset = 0;
    return;
  }
  l.on = true;
  l.inc = 1.0 / (4.0 * std::max(1, l.speed));
}

void ZmdPlayer::UpdateLfo(Track& t, Lfo& l, bool pitch) {
  if (!l.on) return;
  if (l.wait > 0) {
    l.wait--;
    return;
  }
  l.phase += l.inc;
  if (l.phase >= 1.0) l.phase -= 1.0;
  double tri;
  if (l.wave == 1) {
    tri = (l.phase < 0.5) ? 1.0 : -1.0;
  } else if (l.wave == 2) {
    tri = 1.0 - 4.0 * fabs(l.phase - 0.25);
  } else {
    tri = 2.0 * l.phase - 1.0;
  }
  const int v = (int)llround((double)l.depth * tri);
  if (v == l.last_value) return;
  l.last_value = v;
  if (pitch) {
    if (t.key_on) SetPitch(t, t.note, v);
  } else {
    t.amod_offset = v;
    ApplyVolume(t);
  }
}

void ZmdPlayer::Tick() {
  total_seconds_ += TickSeconds();
  for (size_t i = 0; i < tracks_.size(); i++) {
    if (!tracks_[i].dead) TickTrack(tracks_[i]);
  }
  if (!all_dead_ && AllDead()) {
    all_dead_ = true;
    tail_remaining_ = sample_rate_ / 2;
    tail_started_ = true;
  }
}

void ZmdPlayer::TickTrack(Track& t) {
  if (t.gate_wait > 0) {
    t.gate_wait--;
    if (t.gate_wait == 0) KeyOff(t);
  }
  if (t.channel < 8 && t.key_on) {
    UpdateLfo(t, t.pmod, true);
    UpdateLfo(t, t.amod, false);
  }
  if (t.step_wait > 0) {
    t.step_wait--;
    if (t.step_wait == 0) RunCommands(t);
  }
}

void ZmdPlayer::RunCommands(Track& t) {
  int guard = 0;
  while (!t.dead) {
    if (++guard > 100000) {
      EndTrack(t);
      return;
    }
    if (t.pc >= data_.size()) {
      EndTrack(t);
      return;
    }
    const uint8_t* d = data_.data();
    const size_t n = data_.size();
    const uint8_t c = d[t.pc];

    if (c < 0x80) {
      if (t.pc + 3 > n) {
        EndTrack(t);
        return;
      }
      // A note command has no opcode byte: note, step, gate (3 bytes).
      const int note = d[t.pc];
      const int step = d[t.pc + 1];
      const int gate = d[t.pc + 2];
      t.pc += 3;
      HandleNote(t, note, step, gate);
      return;
    }

    switch (c) {
      case 0x80: {
        if (t.pc + 3 > n) {
          EndTrack(t);
          return;
        }
        const int step = d[t.pc + 1];
        const int gate = d[t.pc + 2];
        t.pc += 3;
        HandleRest(t, step, gate);
        return;
      }
      case 0x82:
        t.pc += 1;
        break;
      case 0x83:
        t.pc += 1;
        break;
      case 0x84:
        t.pc += 1;
        break;
      case 0x90: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int timer = Be16(t.pc + 1);
        const int bpm = timer < 1024 ? (int)(78125.0 / std::max(1, 1024 - timer)) : tempo_;
        HandleTempo(bpm);
        t.pc += 3;
        break;
      }
      case 0x91: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        HandleTempo(Be16(t.pc + 1));
        t.pc += 3;
        break;
      }
      case 0x92:
      case 0x93: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        // @T+ / @T-: convert the relative timer value to a relative tempo is
        // not exact, so treat it as a small tempo change.
        const int v = Be16(t.pc + 1);
        HandleTempo(tempo_ + (c == 0x92 ? v : -v));
        t.pc += 3;
        break;
      }
      case 0x94:
      case 0x95: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int v = Be16(t.pc + 1);
        HandleTempo(tempo_ + (c == 0x94 ? v : -v));
        t.pc += 3;
        break;
      }
      case 0x96:
      case 0x97: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int v = Be16(t.pc + 1);
        t.detune_f = Clamp(t.detune_f + (c == 0x96 ? v : -v), -7680, 7680);
        if (t.key_on) SetPitch(t, t.note, 0);
        t.pc += 3;
        break;
      }
      case 0x98: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int pw = (int8_t)d[t.pc + 1];
        const int aw = (int8_t)d[t.pc + 2];
        if (pw >= 0 && pw <= 3) t.pmod.wave = pw;
        if (aw >= 0 && aw <= 3) t.amod.wave = aw;
        RecalcPitchLfo(t);
        RecalcAmpLfo(t);
        t.pc += 3;
        break;
      }
      case 0x99:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pc += 3;
        break;
      case 0x9a:
        if (t.pc + 4 > n) { EndTrack(t); return; }
        t.pc += 4;
        break;
      case 0x9b:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        has_adpcm_ = true;
        t.note = Be16(t.pc + 1);
        t.key_on = true;
        StartAdpcm(t.note);
        t.pc += 3;
        break;
      case 0x9c:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pc += 3;
        break;
      case 0xa0:
      case 0xa1: {
        if (t.pc + 2 > n) { EndTrack(t); return; }
        const int pgm = d[t.pc + 1] - 1;
        if (pgm >= 0 && pgm < (int)voices_.size()) {
          WriteVoice(t, pgm, c == 0xa1);
        }
        t.pc += 2;
        break;
      }
      case 0xa2:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        has_adpcm_ = true;
        adpcm_freq_ = Clamp(d[t.pc + 1], 0, 4);
        t.pc += 2;
        break;
      case 0xa3: {
        if (t.pc + 2 > n) { EndTrack(t); return; }
        const int ch = d[t.pc + 1];
        if (ch >= 1 && ch <= 7) t.channel = ch - 1;
        t.pc += 2;
        break;
      }
      case 0xa5:
      case 0xa6:
      case 0xa7:
      case 0xa8:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xa9:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        has_adpcm_ = true;
        adpcm_freq_ = Clamp(d[t.pc + 1], 0, 4);
        t.pc += 2;
        break;
      case 0xaa:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(t.volume - d[t.pc + 1], 0, 127);
        if (t.channel >= 8) adpcm_track_volume_ = t.volume; else ApplyVolume(t);
        t.pc += 2;
        break;
      case 0xab:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(t.volume + d[t.pc + 1], 0, 127);
        if (t.channel >= 8) adpcm_track_volume_ = t.volume; else ApplyVolume(t);
        t.pc += 2;
        break;
      case 0xac:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xad:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        KeyOn(t, d[t.pc + 1]);
        t.step_wait = 1;
        t.gate_wait = 1;
        t.pc += 2;
        return;
      case 0xae:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.amod.depth = (int8_t)d[t.pc + 1];
        RecalcAmpLfo(t);
        t.pc += 2;
        break;
      case 0xaf:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xb0:
      case 0xb1:
      case 0xb2:
      case 0xb3:
        t.pan = c & 3;
        if (t.channel >= 8) {
          adpcm_pan_ = t.pan;
        } else {
          ApplyPan(t);
        }
        t.pc += 1;
        break;
      case 0xb4: {
        if (t.pc + 2 > n) { EndTrack(t); return; }
        const uint8_t v = d[t.pc + 1];
        t.pan = (v <= 31) ? 1 : (v <= 95 ? 3 : 2);
        if (t.channel >= 8) {
          adpcm_pan_ = t.pan;
        } else {
          ApplyPan(t);
        }
        t.pc += 2;
        break;
      }
      case 0xb5: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        WriteReg(d[t.pc + 1], d[t.pc + 2]);
        t.pc += 3;
        break;
      }
      case 0xb6:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(d[t.pc + 1], 0, 127);
        if (t.channel >= 8) {
          adpcm_track_volume_ = t.volume;
        } else {
          ApplyVolume(t);
        }
        t.pc += 2;
        break;
      case 0xb7:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        has_adpcm_ = true;
        adpcm_pan_ = d[t.pc + 1] & 3;
        t.pc += 2;
        break;
      case 0xb8:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        has_adpcm_ = true;
        t.pc += 2;
        break;
      case 0xb9:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(127 - d[t.pc + 1], 0, 127);
        if (t.channel >= 8) adpcm_track_volume_ = t.volume; else ApplyVolume(t);
        t.pc += 2;
        break;
      case 0xbb:
      case 0xbc:
      case 0xbd:
      case 0xbe:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xbf:
        KeyOff(t);
        t.pc += 1;
        break;
      case 0xc0: {
        if (t.pc + 2 > n) { EndTrack(t); return; }
        const uint8_t sub = d[t.pc + 1];
        const size_t after = t.pc + 2;
        t.pc = after;
        if (sub == 9) {  // [DO]
          t.has_do = true;
          t.do_ptr = after;
          t.loop_pass = 0;
        } else if (sub == 10) {  // [LOOP]
          if (!t.has_do) {
            EndTrack(t);
            return;
          }
          if (loop_limit_ > 0 && t.loop_pass + 1 >= loop_limit_) {
            EndTrack(t);
            return;
          }
          t.loop_pass++;
          t.pc = t.do_ptr;
        } else if (sub == 8) {  // [FINE]
          EndTrack(t);
          return;
        } else if (sub == 3 || sub == 5) {
          // D.C. / D.S. are not used by the target files.
          EndTrack(t);
          return;
        }
        break;
      }
      case 0xc1: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int count = d[t.pc + 2];
        Repeat r;
        r.count = count > 0 ? count : 1;
        r.iter = 1;
        if (t.repeats.size() < 8) t.repeats.push_back(r);
        t.pc += 3;
        break;
      }
      case 0xc2: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int off = Be16(t.pc + 1);
        const size_t after = t.pc + 3;
        if (t.repeats.empty()) {
          t.pc = after;
          break;
        }
        Repeat& r = t.repeats.back();
        if (r.iter < r.count) {
          r.iter++;
          const size_t loop_cf = after - off;
          if (loop_cf < n) {
            t.pc = loop_cf + 2;  // skip $CF and the count byte
          } else {
            t.pc = after;
          }
        } else {
          t.repeats.pop_back();
          t.pc = after;
        }
        break;
      }
      case 0xc3: {
        if (t.pc + 4 > n) { EndTrack(t); return; }
        const int spec = d[t.pc + 1];
        const int off = Be16(t.pc + 2);
        const size_t after = t.pc + 4;
        const int cur = t.repeats.empty() ? -1 : t.repeats.back().iter;
        t.pc = (cur != spec) ? after + off : after;
        break;
      }
      case 0xc4: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int off = Be16(t.pc + 1);
        const size_t after = t.pc + 3;
        if (!t.repeats.empty() && t.repeats.back().iter >= t.repeats.back().count) {
          t.repeats.pop_back();
          t.pc = after + off;
        } else {
          t.pc = after;
        }
        break;
      }
      case 0xc5:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xc7:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xc8:
      case 0xc9:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xca:
      case 0xcb:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(t.volume + (c == 0xca ? -d[t.pc + 1] : d[t.pc + 1]), 0, 127);
        if (t.channel >= 8) adpcm_track_volume_ = t.volume; else ApplyVolume(t);
        t.pc += 2;
        break;
      case 0xcc:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xcd:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xce:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.pc += 2;
        break;
      case 0xd0:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.step_wait = std::max(1, (int)d[t.pc + 1]);
        t.gate_wait = 0;
        t.pc += 3;
        return;
      case 0xd1:
        if (t.pc + 5 > n) { EndTrack(t); return; }
        t.detune_f = (int16_t)Be16(t.pc + 1);
        t.detune_m = (int16_t)Be16(t.pc + 3);
        if (t.key_on) SetPitch(t, t.note, 0);
        t.pc += 5;
        break;
      case 0xd2:
        if (t.pc + 5 > n) { EndTrack(t); return; }
        t.pc += 5;
        break;
      case 0xd3:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pc += 3;
        break;
      case 0xd5:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pc += 3;
        break;
      case 0xd6: {
        if (t.pc + 5 > n) { EndTrack(t); return; }
        const int ps = Be16(t.pc + 1);
        const int as = Be16(t.pc + 3);
        if (ps != 0) {
          t.pmod.speed = std::max(1, ps * 2);
          RecalcPitchLfo(t);
        }
        if (as != 0) {
          t.amod.speed = std::max(1, as * 2);
          RecalcAmpLfo(t);
        }
        t.pc += 5;
        break;
      }
      case 0xd7:
      case 0xd8:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pc += 3;
        break;
      case 0xd9:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(127 - d[t.pc + 1], 0, 127);
        if (t.channel >= 8) adpcm_track_volume_ = t.volume; else ApplyVolume(t);
        t.pc += 2;
        break;
      case 0xda:
      case 0xdb:
        if (t.pc + 2 > n) { EndTrack(t); return; }
        t.volume = Clamp(t.volume + (c == 0xda ? -d[t.pc + 1] : d[t.pc + 1]), 0, 127);
        if (t.channel >= 8) adpcm_track_volume_ = t.volume; else ApplyVolume(t);
        t.pc += 2;
        break;
      case 0xe0:
      case 0xe1:
        if (t.pc + 12 > n) { EndTrack(t); return; }
        t.pc += 12;
        break;
      case 0xe2:
        if (t.pc + 14 > n) { EndTrack(t); return; }
        t.pc += 14;
        break;
      case 0xe3:
        if (t.pc + 9 > n) { EndTrack(t); return; }
        t.pc += 9;
        break;
      case 0xe6:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pmod.depth = (int16_t)Be16(t.pc + 1);
        RecalcPitchLfo(t);
        t.pc += 3;
        break;
      case 0xe8: {
        if (t.pc + 5 > n) { EndTrack(t); return; }
        const int pd = Be16(t.pc + 1);
        const int ad = Be16(t.pc + 3);
        if (pd != 0xffff) t.pmod.delay = pd;
        if (ad != 0xffff) t.amod.delay = ad;
        t.pc += 5;
        break;
      }
      case 0xea: {
        size_t p = t.pc + 1;
        while (p < n && d[p] != 0xff) p++;
        t.pc = (p < n) ? p + 1 : n;
        break;
      }
      case 0xeb:
        if (t.pc + 4 > n) { EndTrack(t); return; }
        t.pc += 4;
        break;
      case 0xec: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int len = Be16(t.pc + 1);
        if (t.pc + 3 + len > n) { EndTrack(t); return; }
        t.pc += 3 + len;
        break;
      }
      case 0xed:
        if (t.pc + 4 > n) { EndTrack(t); return; }
        t.pc += 4;
        break;
      case 0xee:
        if (t.pc + 18 > n) { EndTrack(t); return; }
        t.pc += 18;
        break;
      case 0xef:
        if (t.pc + 10 > n) { EndTrack(t); return; }
        t.pc += 10;
        break;
      case 0xf0:
        t.pc += 1;
        break;
      case 0xf1: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int off = Be16(t.pc + 1);
        t.pc += 3 + off;
        break;
      }
      case 0xf2: {
        if (t.pc + 3 > n) { EndTrack(t); return; }
        const int off = Be16(t.pc + 1);
        t.pc = (t.pc + 3 >= (size_t)off) ? t.pc + 3 - off : 0;
        break;
      }
      case 0xfc:
      case 0xfd:
        if (t.pc + 3 > n) { EndTrack(t); return; }
        t.pc += 3;
        break;
      case 0xfe: {
        if (t.pc + 6 > n) { EndTrack(t); return; }
        const int note = d[t.pc + 1];
        const int step = Be16(t.pc + 2);
        const int gate = Be16(t.pc + 4);
        t.pc += 6;
        HandleNote(t, note, step, gate);
        return;
      }
      case 0xff:
        t.pc += 1;
        EndTrack(t);
        return;
      default:
        // Unknown/unused command: stop instead of running off the rails.
        EndTrack(t);
        return;
    }
  }
}

int ZmdPlayer::Generate(int16_t* out, int frames) {
  if (!started_ || frames <= 0) return 0;
  if (all_dead_ && tail_remaining_ <= 0) return 0;

  if (paused_) {
    // Hold the sequencer but keep the OPM and the ADPCM voice running, like
    // MXDRVG_Pause does for MDX.
    memset(out, 0, (size_t)frames * 4);
    opm_->Mix((FM::Sample*)out, frames);
    MixAdpcm(out, frames);
    return frames;
  }

  int produced = 0;
  while (produced < frames) {
    if (all_dead_) {
      const int n = std::min(frames - produced, tail_remaining_);
      if (n <= 0) break;
      memset(out + produced * 2, 0, (size_t)n * 4);
      opm_->Mix((FM::Sample*)(out + produced * 2), n);
      MixAdpcm(out + produced * 2, n);
      produced += n;
      tail_remaining_ -= n;
      continue;
    }

    int n = frames - produced;
    const double remain = samples_per_tick_ - tick_accum_;
    if (remain > 0.0) {
      const int limit = (int)ceil(remain);
      if (limit < n) n = limit;
    }
    if (n <= 0) {
      tick_accum_ -= samples_per_tick_;
      Tick();
      continue;
    }
    memset(out + produced * 2, 0, (size_t)n * 4);
    opm_->Mix((FM::Sample*)(out + produced * 2), n);
    MixAdpcm(out + produced * 2, n);
    produced += n;
    tick_accum_ += n;
    while (tick_accum_ >= samples_per_tick_) {
      tick_accum_ -= samples_per_tick_;
      Tick();
      if (all_dead_) break;
    }
  }
  return produced;
}

bool ZmdPlayer::IsFinished() const {
  return all_dead_ && tail_remaining_ <= 0;
}

double ZmdPlayer::MeasureSeconds(double max_seconds) {
  Start();
  int guard = 0;
  while (!AllDead() && total_seconds_ < max_seconds) {
    Tick();
    if (++guard > 20000000) break;
  }
  const double result = total_seconds_;
  return result;
}
