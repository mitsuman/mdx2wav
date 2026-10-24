#include "vgm_player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace {

uint32_t ReadU32(const std::vector<uint8_t>& d, size_t off) {
  if (off + 4 > d.size()) return 0;
  return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
         ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
}

// F-Number to MIDI note for the YM2612 at the given clock.
int FnumToNote(int fnum, int block, int clock) {
  if (fnum <= 0 || block < 0 || block > 7) return -1;
  const double freq = (double)fnum * (double)clock /
                      (144.0 * (double)(1u << (20 - block)));
  if (freq <= 0.0) return -1;
  const double note = 69.0 + 12.0 * log2(freq / 440.0);
  if (note < 0.0 || note > 127.0) return -1;
  return (int)(note + 0.5);
}

}  // namespace

bool VGMPlayer::Load(const char* path, std::string* error) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    if (error) *error = "Cannot open VGM file";
    return false;
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    if (error) *error = "Empty VGM file";
    return false;
  }
  data_.resize((size_t)size);
  const size_t got = fread(data_.data(), 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) {
    if (error) *error = "Cannot read VGM file";
    return false;
  }
  if (data_.size() < 0x40 || memcmp(data_.data(), "Vgm ", 4) != 0) {
    if (error) *error = "Not a VGM file";
    return false;
  }

  const uint32_t version = ReadU32(data_, 0x08);
  data_offset_ = 0x34 + ReadU32(data_, 0x34);
  if (data_offset_ >= data_.size()) {
    if (error) *error = "Bad VGM data offset";
    return false;
  }
  total_samples_ = ReadU32(data_, 0x18);
  loop_samples_ = ReadU32(data_, 0x20);
  const uint32_t loop_rel = ReadU32(data_, 0x1C);
  loop_offset_ = loop_rel ? 0x1C + loop_rel : 0;

  opn2_clock_ = (int)ReadU32(data_, 0x2C);
  const int psg_clock = (int)ReadU32(data_, 0x0C);
  has_opn2_ = opn2_clock_ > 0;
  has_psg_ = psg_clock > 0;

  if (!has_opn2_ && !has_psg_) {
    if (error) *error = "VGM uses chips this player does not emulate";
    return false;
  }

  OPN2_Reset(&opn2_);
  OPN2_SetChipType(ym3438_mode_ym2612);
  psg_.Reset(psg_clock > 0 ? psg_clock : 3579545);

  // Title: the file name (GD3 parsing is not implemented).
  const char* base = strrchr(path, '/');
  title_ = base ? base + 1 : path;
  const size_t dot = title_.find_last_of('.');
  if (dot != std::string::npos) title_.erase(dot);

  duration_ = total_samples_ > 0 ? (double)total_samples_ / 44100.0 : 0.0;
  (void)version;
  return true;
}

void VGMPlayer::Start(int loop, double max_duration, double fade_seconds) {
  loop_ = loop > 0 ? loop : 1;
  fade_ = fade_seconds > 0.0 ? fade_seconds : 0.0;
  pos_ = data_offset_;
  wait_ = 0;
  elapsed_ = 0.0;
  finished_ = false;
  chip_phase_ = 0.0;
  last_l_ = last_r_ = 0;
  dc_l_ = dc_r_ = 0.0;
  pending_skip_ = 0;
  OPN2_Reset(&opn2_);
  psg_.Reset(3579545);
  for (int i = 0; i < kFmChannels; i++) {
    wave_pos_[i] = 0;
    memset(wave_ring_[i], 0, sizeof(wave_ring_[i]));
  }
  if (max_duration > 0.0 && (duration_ <= 0.0 || duration_ > max_duration)) {
    duration_ = max_duration;
  }
}

void VGMPlayer::WriteOpn2(int port, uint8_t data) {
  // A write is only applied when the chip's internal slot counter reaches the
  // register's slot, and the next address write would cancel it, so run a full
  // channel cycle (12 clocks) after every byte.  Those cycles are subtracted
  // from the normal per-sample clocks so the song keeps its timing.
  OPN2_Write(&opn2_, (uint32_t)port, data);
  int16_t scratch[2];
  for (int k = 0; k < 12; k++) OPN2_Clock(&opn2_, scratch);
  pending_skip_ += 12;
}

bool VGMPlayer::RunCommand() {
  if (pos_ >= data_.size()) return false;
  const uint8_t c = data_[pos_++];
  switch (c) {
    case 0x50:
      if (pos_ < data_.size()) {
        if (has_psg_) psg_.Write(data_[pos_]);
        pos_++;
      }
      break;
    case 0x52:
      if (pos_ + 1 < data_.size()) {
        if (has_opn2_) {
          // VGM 0x52 carries address + data for chip port 0; the chip's API
          // distinguishes the address latch (port 0) from the data register
          // (port 1).
          WriteOpn2(0, data_[pos_]);
          WriteOpn2(1, data_[pos_ + 1]);
        }
        pos_ += 2;
      } else {
        pos_ = data_.size();
      }
      break;
    case 0x53:
      if (pos_ + 1 < data_.size()) {
        if (has_opn2_) {
          WriteOpn2(2, data_[pos_]);
          WriteOpn2(3, data_[pos_ + 1]);
        }
        pos_ += 2;
      } else {
        pos_ = data_.size();
      }
      break;
    case 0x54:
    case 0x55:
    case 0x5A:
    case 0x5B:
    case 0xA0:
      pos_ += 2;
      break;
    case 0x4F:
      pos_ += 1;
      break;
    case 0x61:
      if (pos_ + 1 < data_.size()) {
        wait_ = (int)data_[pos_] | ((int)data_[pos_ + 1] << 8);
        pos_ += 2;
      }
      break;
    case 0x62:
      wait_ = 735;
      break;
    case 0x63:
      wait_ = 882;
      break;
    case 0x66: {
      // End of the data block: loop when requested and still within the song.
      const bool loop = loop_offset_ != 0 && loop_ > 0 &&
                        (duration_ <= 0.0 || elapsed_ < duration_ - 0.05);
      if (loop) {
        pos_ = loop_offset_;
      } else {
        finished_ = true;
      }
      break;
    }
    case 0x67: {
      // Data block: 0x67 0x66 type size(4) data
      if (pos_ + 6 <= data_.size()) {
        const uint32_t size = ReadU32(data_, pos_ + 2);
        pos_ += 6 + size;
      } else {
        pos_ = data_.size();
      }
      break;
    }
    case 0x68: {
      // PCM RAM write: 0x68 0x66 chip addr(3) ... (rare, skip conservatively)
      if (pos_ + 6 <= data_.size()) {
        pos_ += 6;
      } else {
        pos_ = data_.size();
      }
      break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
      wait_ = (c & 0x0F) + 1;
      break;
    default:
      if (c >= 0x80 && c <= 0x8F) {
        // YM2612 DAC write plus a short wait.
        if (has_opn2_) {
          WriteOpn2(0, 0x2A);
          WriteOpn2(1, (uint8_t)((c & 0x0F) << 4));
        }
        wait_ = c & 0x0F;
      } else if (c >= 0x30 && c <= 0x3F) {
        pos_ += 1;
      } else if ((c >= 0x40 && c <= 0x4E) || (c >= 0x56 && c <= 0x5F) ||
                 (c >= 0xB0 && c <= 0xBF)) {
        pos_ += 2;
      } else if (c >= 0x90 && c <= 0x95) {
        pos_ += 4;
      } else if (c == 0x4F) {
        pos_ += 1;
      }
      break;
  }
  return true;
}

void VGMPlayer::ClockChips() {
  if (has_opn2_) {
    // The chip multiplexes its six channels over 24 master cycles, so one
    // output sample is the sum of all 24 clocks (not just the last one).
    int acc_l = 0, acc_r = 0;
    int16_t buf[2] = {0, 0};
    int clocks = 24;
    if (pending_skip_ > 0) {
      const int skip = pending_skip_ < clocks ? pending_skip_ : clocks;
      clocks -= skip;
      pending_skip_ -= skip;
    }
    for (int k = 0; k < clocks; k++) {
      OPN2_Clock(&opn2_, buf);
      acc_l += buf[0];
      acc_r += buf[1];
    }
    last_l_ = (int16_t)std::max(-32768, std::min(32767, acc_l));
    last_r_ = (int16_t)std::max(-32768, std::min(32767, acc_r));
  }
}

int VGMPlayer::Generate(int16_t* out, int frames) {
  if (!out || frames <= 0) return 0;
  if (finished_) return 0;

  const double chip_rate = has_opn2_ ? (double)opn2_clock_ / 144.0 : 44100.0;
  const double chip_step = chip_rate / 44100.0;

  for (int i = 0; i < frames; i++) {
    while (wait_ == 0 && !finished_) {
      if (!RunCommand()) finished_ = true;
    }
    if (finished_) break;
    wait_--;

    chip_phase_ += chip_step;
    while (chip_phase_ >= 1.0) {
      ClockChips();
      chip_phase_ -= 1.0;
    }

    // Capture each channel's own output once per output sample (ch_out is the
    // channel accumulator, i.e. the value that channel contributes).
    if (has_opn2_) {
      for (int ch = 0; ch < kFmChannels; ch++) {
        // ch_out is a small accumulator value (about +-256); scale it so the
        // panel's auto scale has something to work with.
        int v = opn2_.ch_out[ch] * 64;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        wave_ring_[ch][wave_pos_[ch]++ & (kWaveformSamples - 1)] = (int16_t)v;
      }
    }

    int16_t psg_l = 0, psg_r = 0;
    if (has_psg_) psg_.Generate(&psg_l, &psg_r);

    // Nuked's YM2612 output is a small 9-bit value; scale it into range.
    int l = last_l_ * 8 + psg_l;
    int r = last_r_ * 8 + psg_r;
    // The YM2612's DAC emulation leaves a DC bias; remove it.
    dc_l_ += ((double)l - dc_l_) * 0.0005;
    dc_r_ += ((double)r - dc_r_) * 0.0005;
    l -= (int)dc_l_;
    r -= (int)dc_r_;
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;

    if (fade_ > 0.0 && duration_ > 0.0 && elapsed_ > duration_ - fade_) {
      double g = (duration_ - elapsed_) / fade_;
      if (g < 0.0) g = 0.0;
      l = (int)(l * g);
      r = (int)(r * g);
    }

    out[i * 2 + 0] = (int16_t)l;
    out[i * 2 + 1] = (int16_t)r;
    elapsed_ += 1.0 / 44100.0;
    if (duration_ > 0.0 && elapsed_ >= duration_) finished_ = true;
  }

  if (finished_) {
    // Silence the rest of this buffer.
    for (int i = 0; i < frames; i++) {
      out[i * 2 + 0] = 0;
      out[i * 2 + 1] = 0;
    }
    return 0;
  }
  return frames;
}

void VGMPlayer::GetPsgStates(SN76489::ChannelState out[SN76489::kChannels]) const {
  psg_.GetStates(out);
}

void VGMPlayer::GetVoiceStates(VoiceState out[kFmChannels]) const {
  for (int ch = 0; ch < kFmChannels; ch++) {
    VoiceState& s = out[ch];
    memset(&s, 0, sizeof(s));
    if (!has_opn2_) continue;
    s.fnum = opn2_.fnum[ch];
    s.block = opn2_.block[ch];
    s.algorithm = opn2_.connect[ch];
    s.feedback = opn2_.fb[ch];
    s.pan_left = opn2_.pan_l[ch];
    s.pan_right = opn2_.pan_r[ch];
    s.key_on = opn2_.eg_kon[ch] ? 1 : 0;
    s.note = FnumToNote(s.fnum, s.block, opn2_clock_);
    for (int op = 0; op < 4; op++) {
      s.eg_out[op] = opn2_.eg_out[ch + op * 6];
    }
    s.active = s.key_on != 0 || opn2_.eg_state[ch] != 3;
    if (s.active) {
      const int count = kWaveformSamples;
      for (int j = 0; j < count; j++) {
        s.waveform[j] =
            wave_ring_[ch][(wave_pos_[ch] - count + j) & (count - 1)];
      }
      s.waveform_samples = count;
    }
  }
}
