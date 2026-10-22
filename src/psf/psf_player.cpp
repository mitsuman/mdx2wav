#include "psf_player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "../../gamdx/psf/psflib.h"
#include "../../gamdx/psf/he/psx.h"
#include "../../gamdx/psf/he/bios.h"
#include "../../gamdx/psf/he/iop.h"
#include "../../gamdx/psf/he/r3000.h"
#include "../../gamdx/psf/he/spu.h"
#include "../../gamdx/psf/he/spucore.h"

#include "../spc/spc_resampler.h"

namespace {

const int kOutputRate = 44100;
const int kDefaultDuration = 300;  // seconds, when the file has no length tag

// The PSX core is a singleton: psx_init() sets up global tables.
bool g_core_initialized = false;

void* FileOpen(void* ctx, const char* path) {
  (void)ctx;
  return fopen(path, "rb");
}
size_t FileRead(void* buffer, size_t size, size_t count, void* handle) {
  return fread(buffer, size, count, (FILE*)handle);
}
int FileSeek(void* handle, int64_t offset, int whence) {
  return fseek((FILE*)handle, (long)offset, whence);
}
int FileClose(void* handle) {
  return fclose((FILE*)handle);
}
long FileTell(void* handle) {
  return ftell((FILE*)handle);
}

struct TagSink {
  PSFPlayer* player;
  std::string title;
  std::string game;
  std::string artist;
  double length;
  double fade;
  TagSink() : player(nullptr), length(0.0), fade(0.0) {}
};

// Tag values arrive as "key=value" lines; some files use literal "\n" escapes.
void ApplyTag(TagSink* sink, const std::string& line) {
  std::string text = line;
  if (text.compare(0, 4, "tag ") == 0) text.erase(0, 4);
  const size_t eq = text.find('=');
  if (eq == std::string::npos) return;
  std::string key = text.substr(0, eq);
  std::string value = text.substr(eq + 1);
  while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();

  if (key == "title") {
    sink->title = value;
  } else if (key == "game") {
    sink->game = value;
  } else if (key == "artist") {
    sink->artist = value;
  } else if (key == "length") {
    // "m:ss" or plain seconds
    const size_t colon = value.find(':');
    if (colon != std::string::npos) {
      const double minutes = atof(value.substr(0, colon).c_str());
      const double seconds = atof(value.substr(colon + 1).c_str());
      sink->length = minutes * 60.0 + seconds;
    } else {
      sink->length = atof(value.c_str());
    }
  } else if (key == "fade") {
    sink->fade = atof(value.c_str());
  }
}

int LoadCallback(void* ctx, const uint8_t* exe, size_t exe_size,
                 const uint8_t* reserved, size_t reserved_size) {
  PSFPlayer* player = (PSFPlayer*)ctx;
  (void)reserved;
  (void)reserved_size;
  if (exe_size > 0) {
    // The player owns the chunk list; use a small friend-free interface.
    player->AddExecutable(exe, exe_size);
  }
  return 0;
}

int InfoCallback(void* ctx, const char* name, const char* value) {
  TagSink* sink = (TagSink*)ctx;
  if (!name || !value) return 0;

  // psflib hands over one name/value pair at a time.  Some files put the
  // whole tag block in one value with literal "\n" escapes, so handle both.
  std::string block = std::string(name) + "=" + value;
  std::string line;
  for (size_t i = 0; i < block.size(); i++) {
    if (block[i] == '\n') {
      ApplyTag(sink, line);
      line.clear();
    } else if (block[i] == '\\' && i + 1 < block.size() && block[i + 1] == 'n') {
      ApplyTag(sink, line);
      line.clear();
      i++;
    } else {
      line.push_back(block[i]);
    }
  }
  if (!line.empty()) ApplyTag(sink, line);

  return 0;
}

std::string HomePath(const char* rel) {
  const char* home = getenv("HOME");
  if (!home) return std::string();
  return std::string(home) + "/" + rel;
}

bool ReadWholeFile(const char* path, std::vector<uint8_t>* out) {
  if (!path || !out) return false;
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  const long size = ftell(f);
  if (size <= 0) { fclose(f); return false; }
  rewind(f);
  out->resize((size_t)size);
  const size_t got = fread(out->data(), 1, out->size(), f);
  fclose(f);
  return got == out->size();
}

}  // namespace

namespace {

// PS1 VAG/ADPCM: 16-byte blocks, 28 samples each.  The predictor
// coefficients are the ones used by the SPU core.
const int kVagCoef1[5] = {0, 60, 115, 98, 122};
const int kVagCoef2[5] = {0, 0, -52, -55, -60};

// Decode up to max_samples of the sample that starts at `addr` (a byte address
// in SPU RAM).
int DecodeVag(const uint16_t* ram, int addr, int16_t* out, int max_samples) {
  if (!ram || addr <= 0) return 0;
  int written = 0;
  int p1 = 0, p2 = 0;
  for (int block = 0; block < 256 && written + 28 <= max_samples; block++) {
    const int header = ram[(addr >> 1) & 0x1FFFF];
    const int shift = header & 0x0F;
    const int filter = (header >> 4) & 0x07;
    const int coef1 = kVagCoef1[filter > 4 ? 4 : filter];
    const int coef2 = kVagCoef2[filter > 4 ? 4 : filter];

    for (int word = 0; word < 7; word++) {
      const int data = ram[((addr >> 1) + 1 + word) & 0x1FFFF];
      for (int nib = 0; nib < 4; nib++) {
        int v = (data >> (nib * 4)) & 0x0F;
        if (v >= 8) v -= 16;
        v = (v << 12) >> shift;  // PS1 ADPCM: 12-bit sample with a shift
        v += ((coef1 * p2) + (coef2 * p1) + 32) >> 6;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[written++] = (int16_t)v;
        p1 = p2;
        p2 = v;
      }
    }

    const bool end = (header & 0x0100) != 0;
    addr += 16;
    if (end) break;
  }
  return written;
}

// Estimate the note of a decoded sample at 44.1 kHz with a normalized
// autocorrelation.  Returns 0 for silence and unpitched material.
float EstimateVagTune(const int16_t* samples, int count) {
  if (!samples || count < 128) return 0.0f;

  double mean = 0.0;
  for (int i = 0; i < count; i++) mean += samples[i];
  mean /= count;

  const int max_lag = std::min(count / 2, 512);
  if (max_lag < 8) return 0.0f;

  double best = 0.0;
  std::vector<double> norms(max_lag + 1, 0.0);
  for (int lag = 8; lag <= max_lag; lag++) {
    double corr = 0.0, ea = 0.0, eb = 0.0;
    for (int i = 0; i + lag < count; i++) {
      const double a = samples[i] - mean;
      const double b = samples[i + lag] - mean;
      corr += a * b;
      ea += a * a;
      eb += b * b;
    }
    if (ea > 1e-9 && eb > 1e-9) {
      norms[lag] = corr / sqrt(ea * eb);
      if (norms[lag] > best) best = norms[lag];
    }
  }
  if (best < 0.5) return 0.0f;

  const double threshold = best * 0.85;
  for (int lag = 9; lag < max_lag; lag++) {
    if (norms[lag] >= threshold && norms[lag] >= norms[lag - 1] &&
        norms[lag] >= norms[lag + 1]) {
      const double freq = 44100.0 / (double)lag;
      if (freq < 25.0 || freq > 4000.0) return 0.0f;
      return (float)(69.0 + 12.0 * log(freq / 440.0) / log(2.0));
    }
  }
  return 0.0f;
}

}  // namespace

PSFPlayer::PSFPlayer()
    : state_(nullptr),
      loaded_(false),
      finished_(false),
      sample_rate_(kOutputRate),
      loop_(1),
      max_duration_(0.0),
      duration_(0.0),
      fadeout_seconds_(0.0),
      elapsed_(0.0),
      tag_seconds_(0.0),
      tag_fade_(0.0) {
  for (int i = 0; i < kVoices; i++) {
    key_on_seen_[i] = 0;
    last_wave_pos_[i] = 0;
    wave_stall_[i] = 0;
  }
}

PSFPlayer::~PSFPlayer() {
  if (state_) free(state_);
}

bool PSFPlayer::IsPSFFile(const uint8_t* data, size_t size) {
  return data && size >= 16 && memcmp(data, "PSF", 3) == 0 && data[3] == 1;
}

void PSFPlayer::SetSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 192000) rate = 192000;
  sample_rate_ = rate;
}

std::string PSFPlayer::FindBios(const char* explicit_path) {
  if (explicit_path && explicit_path[0]) return std::string(explicit_path);

  const char* env = getenv("PSF_BIOS");
  if (env && env[0]) return std::string(env);

  const char* candidates[] = {
      "psx-bios.bin", "SCPH1001.BIN", "SCPH1001.bin",
      nullptr,
  };
  for (int i = 0; candidates[i]; i++) {
    FILE* f = fopen(candidates[i], "rb");
    if (f) { fclose(f); return std::string(candidates[i]); }
  }

  const std::string config = HomePath(".config/mdx2wav/psx-bios.bin");
  if (!config.empty()) {
    FILE* f = fopen(config.c_str(), "rb");
    if (f) { fclose(f); return config; }
  }

  // Convenience for the user's own dump directory.
  const std::string home = HomePath("psf/SCPH1001.BIN");
  if (!home.empty()) {
    FILE* f = fopen(home.c_str(), "rb");
    if (f) { fclose(f); return home; }
  }
  return std::string();
}

bool PSFPlayer::LoadBios(const char* path, std::string* error) {
  std::vector<uint8_t> bios;
  if (!ReadWholeFile(path, &bios)) {
    if (error) *error = "Cannot read the PS1 BIOS image";
    return false;
  }
  if (bios.size() < 0x100) {
    if (error) *error = "The PS1 BIOS image is too small";
    return false;
  }

  // The core expects the environment area of the patched "Highly
  // Experimental" BIOS at 0x80.  Write it into our in-memory copy so a stock
  // Sony BIOS can be used; nothing is written to the file on disk.
  static const char kEnv[] =
      "Highly Experimental ps1preboot=52360 ps2preboot=4A44C";
  if ((size_t)0x80 + sizeof(kEnv) <= bios.size()) {
    memset(bios.data() + 0x80, 0, 0x80);
    memcpy(bios.data() + 0x80, kEnv, sizeof(kEnv) - 1);
  }

  bios_ = bios;
  bios_set_image(bios_.data(), (uint32)bios_.size());
  if (!g_core_initialized) {
    if (psx_init() != 0) {
      if (error) *error = "Failed to initialize the PS1 core";
      return false;
    }
    g_core_initialized = true;
  }
  return true;
}

void PSFPlayer::AddExecutable(const uint8_t* data, size_t size) {
  exes_.push_back(std::vector<uint8_t>(data, data + size));
}

void PSFPlayer::ResetCore() {
  if (!state_) return;

  psx_clear_state(state_, 1);

  for (size_t i = 0; i < exes_.size(); i++) {
    psx_upload_psxexe(state_, exes_[i].data(), (uint32)exes_[i].size());
  }

  // The first executable owns the entry point.  In a .psflib set the library
  // holds the driver and the track is only data overlaid on top of it, so the
  // later uploads must not move the program counter.
  uint32_t pc = 0, sp = 0;
  for (size_t i = 0; i < exes_.size(); i++) {
    const std::vector<uint8_t>& exe = exes_[i];
    if (exe.size() < 0x40) continue;
    uint32_t cpc = (uint32_t)exe[0x10] | ((uint32_t)exe[0x11] << 8) |
                   ((uint32_t)exe[0x12] << 16) | ((uint32_t)exe[0x13] << 24);
    uint32_t csp = (uint32_t)exe[0x30] | ((uint32_t)exe[0x31] << 8) |
                   ((uint32_t)exe[0x32] << 16) | ((uint32_t)exe[0x33] << 24);
    if (cpc != 0) {
      pc = cpc;
      sp = csp;
      break;
    }
  }
  if (pc != 0) {
    void* r3 = iop_get_r3000_state(psx_get_iop_state(state_));
    r3000_setreg(r3, R3000_REG_PC, pc);
    if (sp != 0) r3000_setreg(r3, R3000_REG_GEN + 29, sp);
  }
  psx_set_refresh(state_, 60);
}

bool PSFPlayer::Load(const char* path, const char* bios_path, std::string* error) {
  loaded_ = false;
  finished_ = false;
  elapsed_ = 0.0;
  duration_ = 0.0;
  exes_.clear();
  sample_tunes_.clear();
  sample_ends_.clear();
  for (int i = 0; i < kVoices; i++) key_on_seen_[i] = 0;
  title_.clear();
  game_.clear();
  artist_.clear();
  tag_seconds_ = 0.0;
  tag_fade_ = 0.0;
  path_ = path ? path : "";

  std::string bios = FindBios(bios_path);
  if (bios.empty()) {
    if (error) {
      *error =
          "PSF playback needs a PlayStation BIOS image (--psf-bios <file>, "
          "$PSF_BIOS, ./psx-bios.bin or ~/.config/mdx2wav/psx-bios.bin)";
    }
    return false;
  }
  if (!LoadBios(bios.c_str(), error)) return false;

  psf_file_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.path_separators = "/";
  callbacks.context = nullptr;
  callbacks.fopen = FileOpen;
  callbacks.fread = FileRead;
  callbacks.fseek = FileSeek;
  callbacks.fclose = FileClose;
  callbacks.ftell = FileTell;

  TagSink tags;
  tags.player = this;
  const int version = psf_load(path, &callbacks, 1, LoadCallback, this,
                               InfoCallback, &tags, 0, nullptr, nullptr);
  if (version != 1) {
    if (error) *error = "Not a playable PSF1 file";
    return false;
  }
  if (exes_.empty()) {
    if (error) *error = "The PSF file did not contain an executable";
    return false;
  }

  title_ = tags.title;
  game_ = tags.game;
  artist_ = tags.artist;
  tag_seconds_ = tags.length;
  tag_fade_ = tags.fade;

  if (!state_) {
    const uint32_t size = psx_get_state_size(1);
    state_ = malloc(size);
    if (!state_) {
      if (error) *error = "Out of memory for the PS1 state";
      return false;
    }
  }
  ResetCore();

  if (title_.empty()) {
    const char* base = strrchr(path, '/');
    title_ = base ? base + 1 : path;
  }
  loaded_ = true;
  return true;
}

void PSFPlayer::Start(int loop, double max_duration, double fadeout_seconds) {
  loop_ = loop > 0 ? loop : 1;
  max_duration_ = max_duration;
  fadeout_seconds_ = fadeout_seconds > 0.0 ? fadeout_seconds : 0.0;
  elapsed_ = 0.0;
  finished_ = false;

  // PSF files loop through the sequences by themselves; the length tag (plus
  // an optional fade) is the canonical duration.
  double length = tag_seconds_ > 0.0 ? tag_seconds_ : (double)kDefaultDuration;
  if (tag_fade_ > 0.0) length += tag_fade_;
  if (max_duration_ > 0.0 && length > max_duration_) length = max_duration_;
  duration_ = length;

  if (sample_rate_ != kOutputRate) resampler_.Init(kOutputRate, sample_rate_);
  if (loaded_) ResetCore();
}

int PSFPlayer::Generate(int16_t* out, int frames) {
  if (sample_rate_ == kOutputRate) return GenerateCore(out, frames);

  int done = 0;
  while (done < frames) {
    const int got = resampler_.Pull(out + done * 2, frames - done);
    done += got;
    if (done >= frames) break;
    raw_.resize(1024 * 2);
    const int n = GenerateCore(raw_.data(), 1024);
    if (n <= 0) break;
    resampler_.Push(raw_.data(), n);
  }
  return done;
}

int PSFPlayer::GenerateCore(int16_t* out, int frames) {
  if (!loaded_ || !state_ || frames <= 0) return 0;
  if (duration_ > 0.0 && elapsed_ >= duration_) {
    finished_ = true;
    return 0;
  }

  int produced = 0;
  while (produced < frames) {
    int want = frames - produced;
    if (want > 1024) want = 1024;
    uint32_t samples = (uint32_t)want;
    sint32 r = psx_execute(state_, 0x70000000, out + produced * 2, &samples, 0);
    if (r < -1) {
      finished_ = true;
      break;
    }
    if (samples == 0) {
      finished_ = true;
      break;
    }
    produced += (int)samples;
  }

  // Fade out over the tail of the song.
  if (fadeout_seconds_ > 0.0 && duration_ > 0.0) {
    const double start = duration_ - fadeout_seconds_;
    const double t0 = elapsed_;
    for (int i = 0; i < produced; i++) {
      const double t = t0 + (double)i / kOutputRate;
      if (t < start) continue;
      double gain = 1.0 - (t - start) / fadeout_seconds_;
      if (gain < 0.0) gain = 0.0;
      out[i * 2] = (int16_t)(out[i * 2] * gain);
      out[i * 2 + 1] = (int16_t)(out[i * 2 + 1] * gain);
    }
  }

  elapsed_ += (double)produced / kOutputRate;
  if (duration_ > 0.0 && elapsed_ >= duration_) finished_ = true;
  return produced;
}

void PSFPlayer::GetVoiceStates(VoiceState out[kVoices]) const {
  if (!out) return;
  memset(out, 0, sizeof(VoiceState) * kVoices);
  if (!loaded_ || !state_) return;

  void* iop = psx_get_iop_state(state_);
  void* spu = iop_get_spu_state(iop);

  const uint32_t kon = spu_getreg_main(spu, SPUREG_KON);
  for (int v = 0; v < kVoices; v++) {
    VoiceState& s = out[v];
    s.pitch = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_PITCH);
    s.volume_left = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_VOLXL);
    s.volume_right = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_VOLXR);
    s.envelope = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_ENVX);
    s.adsr1 = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_ADSR1);
    s.adsr2 = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_ADSR2);
    s.phase = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_ENVSTATE);
    const uint32_t ons = (uint32_t)spu_getreg_voice(spu, v, SPUREG_VOICE_KEYONS);
    s.note_on = ons != key_on_seen_[v];
    if (s.note_on) key_on_seen_[v] = ons;
    s.start_address = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_SSA);
    s.loop_address = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_LSAX);
    s.current_address = (int)spu_getreg_voice(spu, v, SPUREG_VOICE_NAX);
    if (s.start_address > 0) {
      // The playback-position meter follows the region that is actually
      // playing.  The loop-start register is hardware-updated and therefore
      // reliable once the voice has reached its loop point, while SSA can
      // already hold the next sample; prefer the loop start when it contains
      // the current address.
      const uint16_t* ram = spu_get_ram(spu);
      int start = s.start_address;
      if (s.loop_address > 0 && s.loop_address <= s.current_address &&
          s.loop_address < 0x80000) {
        start = s.loop_address;
      }
      auto findEnd = [&](int from) -> int {
        auto it = sample_ends_.find(from);
        if (it != sample_ends_.end()) return it->second;
        int addr = from;
        for (int block = 0; block < 4096; block++) {
          const int header = ram[(addr >> 1) & 0x1FFFF];
          addr += 16;
          if (header & 0x0100) break;
        }
        sample_ends_[from] = addr;
        return addr;
      };
      int end = findEnd(start);
      if (s.current_address < start || s.current_address > end) {
        start = s.start_address;
        end = findEnd(start);
      }
      s.sample_start = start;
      s.sample_end_address = end;
    }
    // The KON register bits stay set after a voice finishes, and ENVX drops to
    // 0 while a quiet release is still running, so use the envelope state.
    s.active = s.phase != 0;
    s.waveform_samples = 0;

    if (s.active) {
      // Copy the captured per-voice output (the actual level used in the mix).
      const int ring_size = 2048;
      const int16_t* ring = spu_dbg_waveform(spu, (uint32)v);
      const uint32 pos = spu_dbg_wave_pos(spu, (uint32)v);
      const int count = std::min((int)kWaveformSamples, ring_size);
      for (int j = 0; j < count; j++) {
        s.waveform[j] = ring[(pos - count + j + ring_size) & (ring_size - 1)];
      }
      s.waveform_samples = count;
      // A voice that no longer feeds the mix (muted, finished or still
      // deferred) would otherwise show the last window forever.
      if (pos == last_wave_pos_[v]) {
        wave_stall_[v]++;
      } else {
        last_wave_pos_[v] = pos;
        wave_stall_[v] = 0;
      }
      if (wave_stall_[v] >= 4) {
        // The voice no longer produces samples (its sample ended or it is not
        // routed), so stop showing it as playing even if the envelope is still
        // decaying.
        s.waveform_samples = 0;
        s.active = false;
      }
    }

    // Note = the sample's own note (at pitch 0x1000) plus the pitch register.
    if (s.active && s.pitch > 0 && s.start_address > 0) {
      auto it = sample_tunes_.find(s.start_address);
      float tune;
      if (it == sample_tunes_.end()) {
        int16_t decoded[1024];
        const uint16_t* ram = spu_get_ram(spu);
        const int count = DecodeVag(ram, s.start_address, decoded, 1024);
        tune = EstimateVagTune(decoded, count);
        sample_tunes_[s.start_address] = tune > 0.0f ? tune : -1.0f;
        if (tune <= 0.0f) tune = 0.0f;
      } else {
        tune = it->second > 0.0f ? it->second : 0.0f;
      }
      if (tune > 0.0f) {
        float n = tune + 12.0f * (float)log2((double)s.pitch / 4096.0);
        if (n < 0.0f) n = 0.0f;
        if (n > 127.0f) n = 127.0f;
        s.note = (int)(n + 0.5f);
      }
    }
  }

}

void PSFPlayer::GetGlobalState(GlobalState* out) const {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!loaded_ || !state_) return;

  void* iop = psx_get_iop_state(state_);
  void* spu = iop_get_spu_state(iop);
  out->master_volume_left = (int)spu_getreg_main(spu, SPUREG_MVOLL);
  out->master_volume_right = (int)spu_getreg_main(spu, SPUREG_MVOLR);
  out->reverb_enabled = (int)spu_getreg_flag(spu, SPUREG_FLAG_REVERB_ENABLE);
  out->main_enabled = (int)spu_getreg_flag(spu, SPUREG_FLAG_MAIN_ENABLE);
  out->key_on_mask = (int)spu_getreg_main(spu, SPUREG_KON);
}
