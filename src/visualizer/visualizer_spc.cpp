#include "visualizer.h"

#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

const char* kPhaseNames[] = {"REL", "ATK", "DEC", "SUS"};

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

}  // namespace

// SPC (SNES S-DSP) channel display.
//
// SPC music is built from eight sample-playback voices rather than FM
// operators, so this replaces the YM2151 operator/algorithm panel with a
// compact per-voice readout.  The keyboard, waveform and spectrum displays are
// shared with the MDX layout.
void Visualizer::renderSPCChannelInfo() {
  if (!state_) return;

  YM2151State::Channel channels[8];
  state_->getAllChannels(channels);

  int y = YM2151_START_Y;
  const int line_height = YM2151_LINE_HEIGHT;

  for (int ch = 0; ch < ym2151_display_channels_ && ch < 8; ch++) {
    const YM2151State::Channel& c = channels[ch];

    int r, g, b;
    getChannelColor(ch, r, g, b);

    const bool active = c.key_on || c.spc_envelope > 0;
    const int seg_r = active ? CHANNEL_LABEL_COLOR_ACTIVE_R : CHANNEL_LABEL_COLOR_INACTIVE_R;
    const int seg_g = active ? CHANNEL_LABEL_COLOR_ACTIVE_G : CHANNEL_LABEL_COLOR_INACTIVE_G;
    const int seg_b = active ? CHANNEL_LABEL_COLOR_ACTIVE_B : CHANNEL_LABEL_COLOR_INACTIVE_B;

    // Voice number and chip labels.
    draw7Segment(20, y + 10, ch, seg_r, seg_g, seg_b, 8, 7);
    renderBitmapText("S-DSP", 10, y + 28, seg_r, seg_g, seg_b);
    renderBitmapText("SPC700", 10, y + 38, seg_r / 2, seg_g / 2, seg_b / 2);

    // Key-on / envelope activity bar next to the voice number.
    const int env_height = 40;
    const int env_y = y + 8;
    SDL_Rect env_bg = {70, env_y, 8, env_height};
    SDL_SetRenderDrawColor(renderer_, 30, 30, 50, 255);
    SDL_RenderFillRect(renderer_, &env_bg);
    SDL_SetRenderDrawColor(renderer_, 60, 70, 110, 255);
    SDL_RenderDrawRect(renderer_, &env_bg);
    int env_level = clampInt(c.spc_envelope, 0, 0x7FF);
    int env_h = (env_level * (env_height - 2)) / 0x7FF;
    if (env_h > 0) {
      SDL_SetRenderDrawColor(renderer_, r, g, b, active ? 255 : 120);
      SDL_Rect env_bar = {71, env_y + env_height - 1 - env_h, 6, env_h};
      SDL_RenderFillRect(renderer_, &env_bar);
    }

    // Note and pitch information.
    char text[96];
    if (!c.key_on || c.note == 0) {
      snprintf(text, sizeof(text), "NOTE  ---");
    } else {
      const int octave = c.note / 12 - 1;
      snprintf(text, sizeof(text), "NOTE %s%d (%d)", getNoteName(c.note), octave, c.note);
    }
    renderBitmapText(text, 88, y + 6, active ? 220 : 100, active ? 230 : 110, 255);

    snprintf(text, sizeof(text), "PIT %04X", c.spc_pitch & 0x3FFF);
    renderBitmapText(text, 88, y + 18, 120, 180, 255);

    snprintf(text, sizeof(text), "ENV %03X", c.spc_envelope & 0x7FF);
    renderBitmapText(text, 88, y + 30, 100, 200, 180);

    const char* phase = kPhaseNames[c.spc_env_phase & 0x03];
    int pr = 100, pg = 100, pb = 140;
    switch (c.spc_env_phase & 0x03) {
      case 1: pr = 100; pg = 255; pb = 100; break;
      case 2: pr = 255; pg = 200; pb = 100; break;
      case 3: pr = 100; pg = 180; pb = 255; break;
      default: pr = 200; pg = 100; pb = 200; break;
    }
    renderBitmapText(phase, 10, y + 53, pr, pg, pb);

    snprintf(text, sizeof(text), "SRC %04X", c.spc_source);
    renderBitmapText(text, 160, y + 18, 120, 150, 200);
    snprintf(text, sizeof(text), "LOOP %04X", c.spc_loop);
    renderBitmapText(text, 160, y + 30, 120, 150, 200);
    snprintf(text, sizeof(text), "BRR %04X", c.spc_brr);
    renderBitmapText(text, 160, y + 42, 90, 120, 180);

    // Voice volume (envelope combined with the L/R volume registers).
    const int vol_width = 130;
    SDL_Rect vol_bg = {88, y + 52, vol_width, 6};
    SDL_SetRenderDrawColor(renderer_, 30, 30, 50, 255);
    SDL_RenderFillRect(renderer_, &vol_bg);
    SDL_SetRenderDrawColor(renderer_, 60, 70, 110, 255);
    SDL_RenderDrawRect(renderer_, &vol_bg);
    int vol_w = (clampInt(c.velocity, 0, 127) * (vol_width - 2)) / 127;
    if (vol_w > 0) {
      SDL_SetRenderDrawColor(renderer_, r, g, b, active ? 230 : 90);
      SDL_Rect vol = {89, y + 53, vol_w, 4};
      SDL_RenderFillRect(renderer_, &vol);
    }

    // L/R indicator.
    const char* pan = "  ";
    if ((c.left_right & 0x80) && (c.left_right & 0x40)) pan = "LR";
    else if (c.left_right & 0x80) pan = "L ";
    else if (c.left_right & 0x40) pan = " R";
    renderBitmapText(pan, 225, y + 53, active ? 150 : 70, active ? 200 : 90, 255);

    // PCM playback position within the BRR sample/loop, like the ADPCM bar.
    if (c.spc_source != 0 && c.spc_end > c.spc_source) {
      const int bar_x = 88;
      const int bar_y = y + 61;
      const int bar_w = 140;
      const int bar_h = 4;
      const int start = c.spc_source;
      const int end = c.spc_end;
      const int span = end - start;
      int pos = c.spc_brr;
      if (pos < start) pos = start;
      if (pos > end) pos = end;
      const int played = (pos - start) * (bar_w - 2) / span;

      SDL_Rect bg = {bar_x, bar_y, bar_w, bar_h};
      SDL_SetRenderDrawColor(renderer_, 30, 30, 50, 255);
      SDL_RenderFillRect(renderer_, &bg);
      SDL_SetRenderDrawColor(renderer_, 60, 70, 110, 255);
      SDL_RenderDrawRect(renderer_, &bg);

      if (played > 0) {
        SDL_SetRenderDrawColor(renderer_, r, g, b, active ? 230 : 90);
        SDL_Rect fill = {bar_x + 1, bar_y + 1, played, bar_h - 2};
        SDL_RenderFillRect(renderer_, &fill);
      }

      // Loop point tick.
      if (c.spc_loop >= start && c.spc_loop <= end) {
        const int loop_x = bar_x + 1 + (c.spc_loop - start) * (bar_w - 2) / span;
        SDL_SetRenderDrawColor(renderer_, 255, 220, 120, 220);
        SDL_RenderDrawLine(renderer_, loop_x, bar_y, loop_x, bar_y + bar_h - 1);
      }

      // Current position marker.
      SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
      SDL_RenderDrawLine(renderer_, bar_x + 1 + played, bar_y,
                         bar_x + 1 + played, bar_y + bar_h - 1);
    }

    // Actual generated output for this voice, captured from the S-DSP.
    int16_t waveform[YM2151State::CHANNEL_WAVEFORM_SIZE];
    state_->getChannelWaveform(ch, waveform, YM2151State::CHANNEL_WAVEFORM_SIZE);
    const int width = channelWaveformWidth();
    const int samples = std::min<int>(c.spc_waveform_samples,
                                      YM2151State::CHANNEL_WAVEFORM_SIZE);
    // The captured per-voice output is usually much quieter than full scale.
    // Apply a fixed 4x display gain with a soft limiter: quiet voices become
    // readable, while loud voices keep their shape instead of clipping flat.
    int16_t display[YM2151State::CHANNEL_WAVEFORM_SIZE];
    const float gain = 4.0f * ym2151_waveform_scale_;
    for (int i = 0; i < samples; i++) {
      const double x = (double)waveform[i] * gain / 32768.0;
      display[i] = (int16_t)std::lrint(32767.0 * std::tanh(x));
    }
    renderChannelWaveform(channelWaveformX(), y + 2, width, line_height - 20,
                          samples > 0 ? display : nullptr, samples,
                          r, g, b, ch, 1.0f, false);

    y += line_height;
  }

  renderSPCStatus(y);
}

// Global S-DSP state at the bottom of the window (master volume, echo, KON and
// CPU program counter).  The browser build always uses the tall MDX window, and
// the native SPC layout does the same, so this panel fills the space below the
// eight voices.
void Visualizer::renderSPCStatus(int y) {
  if (!state_) return;
  if (window_height_ - y < 60) return;

  YM2151State::SPCGlobal g;
  state_->getSPCGlobal(g);

  SDL_Rect panel = {5, y + 4, window_width_ - 10, window_height_ - y - 12};
  SDL_SetRenderDrawColor(renderer_, 16, 18, 40, 255);
  SDL_RenderFillRect(renderer_, &panel);
  SDL_SetRenderDrawColor(renderer_, 60, 70, 120, 255);
  SDL_RenderDrawRect(renderer_, &panel);

  renderBitmapText("S-DSP STATUS", 15, y + 12, 150, 190, 255);

  char text[160];
  snprintf(text, sizeof(text),
           "MASTER %03d/%03d  ECHO VOL %03d/%03d  EFB %03d  EDL %d  ESA %02X",
           g.master_volume_left, g.master_volume_right,
           g.echo_volume_left, g.echo_volume_right,
           g.echo_feedback, g.echo_delay, g.echo_start);
  renderBitmapText(text, 15, y + 26, 130, 170, 220);

  snprintf(text, sizeof(text),
           "KON %02X  DIR %02X  PC %04X  ECHO %04X/%04X  NOISE %s",
           g.key_on_mask & 0xFF, g.directory_address & 0xFF, g.cpu_pc & 0xFFFF,
           g.echo_offset & 0xFFFF, g.echo_length & 0xFFFF,
           g.noise_enable ? "ON" : "OFF");
  renderBitmapText(text, 15, y + 38, 110, 150, 200);

  // Per-voice activity dots.
  int x = 15;
  int dy = y + 52;
  for (int ch = 0; ch < 8; ch++) {
    YM2151State::Channel c;
    state_->getChannelInfo(ch, c);
    int r, g2, b;
    getChannelColor(ch, r, g2, b);
    const bool on = c.key_on || c.spc_envelope > 0;
    SDL_Rect dot = {x, dy, 10, 10};
    SDL_SetRenderDrawColor(renderer_, on ? r : 40, on ? g2 : 40, on ? b : 55, 255);
    SDL_RenderFillRect(renderer_, &dot);
    SDL_SetRenderDrawColor(renderer_, 80, 90, 140, 255);
    SDL_RenderDrawRect(renderer_, &dot);
    char label[8];
    snprintf(label, sizeof(label), "%d", ch);
    renderBitmapText(label, x + 14, dy + 2, on ? 200 : 90, on ? 220 : 100, 255);
    x += 42;
  }
}
