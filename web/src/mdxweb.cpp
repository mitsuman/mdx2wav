// Web (Emscripten) harness for mdx2wav.
//
// This reuses the original MDX driver (MXDRVG + fmgen/mame YM2151 + PCM8) and the
// SDL2 visualizer, but replaces the file/stdout oriented main loop of
// src/mdx2wav.cpp with a step-driven loop that the JavaScript front end pumps:
//   - the browser asks for audio 10 ms at a time and pushes it into a ring buffer
//     which an AudioWorklet drains,
//   - one visualizer frame is rendered per animation frame.
//
// Everything (audio generation and SDL rendering) runs on the Emscripten main
// thread so that SDL2 can talk to the canvas directly.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "gamdx/mxdrvg/mxdrvg.h"
#include "web/src/mdxweb_load.h"
#include "gamdx/mxdrvg/opm_visualizer.h"
#include "gamdx/mxdrvg/pcm8_visualizer.h"
#include "src/visualizer/visualizer.h"
#include "src/visualizer/ym2151_state.h"

#include <emscripten.h>

// Callbacks implemented on the JavaScript side (see web/src/mdxweb.js).
struct WebCallbacks {
  void (*on_log)(void* user, const char* message);
  void (*on_song_end)(void* user);
  void* user;
  const char* filename;
};

namespace {

typedef unsigned char u8;
typedef unsigned int u32;

const int MAGIC_OFFSET = 10;
const int SAMPLE_RATE = 44100;
const int MDX_BUF_SIZE = 256 * 1024;
const int PDX_BUF_SIZE = 1024 * 1024;
const int FILTER_MODE = 0;
const int DEFAULT_LOOP = 2;
const int CHUNK_SAMPLES = SAMPLE_RATE / 100;  // 10ms, matches the native binary

bool g_verbose = false;
// The spectrum analyzers run sixteen FFTs per update, by far the most expensive
// part of a frame, so the browser build leaves them off unless asked.
bool g_spectrum_enabled = false;

std::vector<int16_t> g_audio;   // CHUNK_SAMPLES * 2 interleaved samples
float g_volume = 1.0f;
bool g_swap_channels = false;
int g_loop = DEFAULT_LOOP;
int g_fadeout = 0;

// clang-format off
WebCallbacks g_cb;
// clang-format on

void Log(const char* text) {
  if (g_cb.on_log) {
    g_cb.on_log(g_cb.user, text);
  }
  fprintf(stderr, "%s\n", text);
}

}  // namespace

struct MDXEngine {
  bool initialized = false;
  bool running = false;
  double elapsed = 0.0;
  double duration = 0.0;       // seconds, measured by MXDRVG_MeasurePlayTime
  unsigned long long frames = 0;
  char title[256];

  YM2151State* ym_state = nullptr;
  Visualizer* visualizer = nullptr;
  OPM_Delegate* opm_original = nullptr;
  OPMVisualizer* opm_wrapper = nullptr;
  PCM8Visualizer* pcm8_wrapper = nullptr;
  X68K::X68PCM8* pcm8_original = nullptr;
  bool paused = false;
};

static MDXEngine* E = nullptr;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" {

EMSCRIPTEN_KEEPALIVE
void mdxweb_configure(int volume_percent, int swap_channels, int loop, int fadeout) {
  g_volume = volume_percent / 100.0f;
  g_swap_channels = swap_channels != 0;
  g_loop = loop > 0 ? loop : DEFAULT_LOOP;
  g_fadeout = fadeout;
}

EMSCRIPTEN_KEEPALIVE
void mdxweb_verbose(int enabled) { g_verbose = enabled != 0; }

// Enable or disable the spectrum analyzers (1 = on).  Disabling also stops the
// per-channel FFT work in Visualizer::updateWaveform(), and widens the waveform
// display into the strip the spectrum used to occupy.
EMSCRIPTEN_KEEPALIVE
void mdxweb_set_spectrum(int enabled) {
  g_spectrum_enabled = enabled != 0;
  if (E && E->visualizer) {
    E->visualizer->setSpectrumEnabled(g_spectrum_enabled);
  }
}

// Canvas size the current spectrum setting expects (the width shrinks when the
// spectrum is hidden).  Call before mdxweb_init().
EMSCRIPTEN_KEEPALIVE
int mdxweb_window_width() {
  return Visualizer::windowWidthForSpectrum(g_spectrum_enabled);
}


EMSCRIPTEN_KEEPALIVE
int mdxweb_init(int ym2151_channels, int adpcm_channels, float waveform_scale,
                int width, int height) {
  // Diagnostics must reach the browser console even if the engine aborts.
  setvbuf(stderr, nullptr, _IONBF, 0);

  // Scratch buffer for one 10 ms chunk.  The visualizer is handed g_audio.data()
  // on every step, so it has to hold storage from the very first frame; leaving
  // it to mdxweb_main() left the pointer null and silently disabled the
  // spectrum analyzers.
  if (g_audio.empty()) {
    g_audio.resize(CHUNK_SAMPLES * 2);
  }

  if (!E) {
    E = new MDXEngine();
  }
  if (E->initialized) {
    return 1;
  }

  // init() is called again after mdxweb_shutdown() to rebuild the visualizer
  // when the layout changes, so start from a clean slate.
  E->running = false;
  E->paused = false;
  E->elapsed = 0.0;
  E->frames = 0;
  E->ym_state = nullptr;
  E->visualizer = nullptr;
  E->opm_original = nullptr;
  E->opm_wrapper = nullptr;
  E->pcm8_wrapper = nullptr;
  E->pcm8_original = nullptr;

  MXDRVG_SetEmulationType(MXDRVG_YM2151TYPE_FMGEN);

  if (MXDRVG_Start(SAMPLE_RATE, FILTER_MODE, MDX_BUF_SIZE, PDX_BUF_SIZE) != 0) {
    Log("MXDRVG_Start failed");
    return 0;
  }
  MXDRVG_TotalVolume(256);

  // The visualizer is started after MXDRVG_Start() so that the driver's own
  // default delegates are already in place (same ordering as the native main).
  E->ym_state = new YM2151State();

  E->visualizer = new Visualizer();
  // The spectrum setting decides the layout width, so it has to be in place
  // before the window is created.
  E->visualizer->setSpectrumEnabled(g_spectrum_enabled);

  // The spectrum setting decides the layout width.
  width = Visualizer::windowWidthForSpectrum(g_spectrum_enabled);

  if (!E->visualizer->init("MDX Visualizer", width, height)) {
    Log("Visualizer init failed");
    return 0;
  }
  E->visualizer->setState(E->ym_state);
  E->visualizer->setDisplayChannels(ym2151_channels, adpcm_channels);
  E->visualizer->setWaveformScale(waveform_scale, waveform_scale);

  E->opm_original = MXDRVG_GetOPMDelegate();
  if (E->opm_original) {
    E->opm_wrapper = new OPMVisualizer(E->opm_original, E->ym_state);
    MXDRVG_SetOPMDelegate(E->opm_wrapper);
    E->opm_wrapper->setVisualizer(E->visualizer);
    E->visualizer->setOPMWrapper(E->opm_wrapper);
  }

  E->pcm8_original = MXDRVG_GetPCM8();
  E->pcm8_wrapper = new PCM8Visualizer(E->ym_state);
  MXDRVG_SetPCM8(E->pcm8_wrapper);

  E->initialized = true;
  return 1;
}

// Hand the MDX (and optionally PDX) contents to the driver.  The parsing and
// buffer setup live in mdxweb_load.cpp so the native test can exercise them.
EMSCRIPTEN_KEEPALIVE
int mdxweb_load(const u8* mdx, int mdx_size, const u8* pdx, int pdx_size) {
  if (!E || !E->initialized) {
    return 0;
  }

  E->title[0] = 0;
  if (!mdxweb_load_data(mdx, mdx_size, pdx, pdx_size, E->title, sizeof(E->title))) {
    Log("Failed to load MDX data");
    return 0;
  }

  E->visualizer->setSongTitle(E->title);
  E->visualizer->setFilename(g_cb.filename ? g_cb.filename : "");
  E->visualizer->resetElapsedTime();
  E->visualizer->clearFileChangeRequest();
  E->visualizer->clearRestartRequest();

  E->ym_state->setADPCMBufferInfo(MXDRVG_GetADPCMBuffer(),
                                  MXDRVG_GetADPCMBufferSize());

  E->elapsed = 0.0;
  E->frames = 0;

  // MeasurePlayTime() drives the driver to the end of the song, which also
  // frees/rewinds internal state, so playback has to be restarted afterwards.
  E->duration = MXDRVG_MeasurePlayTime(g_loop, g_fadeout) / 1000.0;
  MXDRVG_PlayAt(0, g_loop, g_fadeout);
  MXDRVG_Cont();

  E->running = true;
  E->paused = false;
  return 1;
}

EMSCRIPTEN_KEEPALIVE
void mdxweb_start() {
  if (!E) return;
  MXDRVG_PlayAt(0, g_loop, g_fadeout);
  MXDRVG_Cont();
  E->running = true;
  E->elapsed = 0.0;
}

EMSCRIPTEN_KEEPALIVE
void mdxweb_stop() {
  if (!E) return;
  E->running = false;
  MXDRVG_Pause();
}

EMSCRIPTEN_KEEPALIVE
void mdxweb_set_paused(int paused) {
  if (!E) return;
  if (paused && !E->paused) {
    E->paused = true;
    MXDRVG_Pause();
  } else if (!paused && E->paused) {
    E->paused = false;
    MXDRVG_Cont();
  }
}

// Generate the next 10 ms of audio.  A visualizer frame is drawn only every
// RENDER_EVERY steps so that audio generation is not throttled by rendering.
EMSCRIPTEN_KEEPALIVE
int mdxweb_step() {
  if (!E || !E->initialized) {
    return 0;
  }

  const int kRenderEvery = 7;  // ~14 Hz at 100 steps/s: enough for the UI
  const bool do_render = (E->frames % kRenderEvery) == 0;

  if (do_render) {
    // Keep the visualizer's view of the FM chip in sync with the driver.
    if (E->opm_original) {
      void* opm_ptr = E->opm_original->GetOPMPointer();
      if (opm_ptr) {
        E->ym_state->updateFromFmgen(opm_ptr);
        E->visualizer->setOPMPointer(opm_ptr);
      }
    }

    // Let SDL process queued input; this also keeps the canvas responsive.
    E->visualizer->update();

    if (E->visualizer->hasRestartRequest()) {
      E->visualizer->clearRestartRequest();
      mdxweb_start();
    }
  }

  if (!E->running) {
    return 0;
  }

  int len = MXDRVG_GetPCM((short*)g_audio.data(), CHUNK_SAMPLES);
  if (len <= 0) {
    E->running = false;
    return 0;
  }

  if (g_swap_channels) {
    for (int i = 0; i < len; i++) {
      short tmp = g_audio[i * 2];
      g_audio[i * 2] = g_audio[i * 2 + 1];
      g_audio[i * 2 + 1] = tmp;
    }
  }
  if (g_volume != 1.0f) {
    for (int i = 0; i < len * 2; i++) {
      int s = (int)(g_audio[i] * g_volume);
      if (s > 32767) s = 32767;
      else if (s < -32768) s = -32768;
      g_audio[i] = (short)s;
    }
  }

  E->visualizer->updateWaveform(g_audio.data(), len);
  E->elapsed += (double)len / SAMPLE_RATE;
  E->frames++;

  if (MXDRVG_GetTerminated()) {
    E->running = false;
  }
  return len;
}

EMSCRIPTEN_KEEPALIVE
short* mdxweb_audio_buffer() { return g_audio.data(); }

// Generate a fixed amount of PCM into `out` without ring buffer / visualizer
// pacing.  Used by the automated tests to compare against the native build.
EMSCRIPTEN_KEEPALIVE
int mdxweb_dump_pcm(short* out, int max_frames) {
  if (!E || !E->initialized || !out) {
    return 0;
  }
  mdxweb_start();
  int written = 0;
  while (written < max_frames) {
    int want = max_frames - written;
    if (want > CHUNK_SAMPLES) want = CHUNK_SAMPLES;
    int len = MXDRVG_GetPCM(out + written * 2, want);
    if (len <= 0) break;
    written += len;
    if (MXDRVG_GetTerminated()) break;
  }
  return written;
}

EMSCRIPTEN_KEEPALIVE
int mdxweb_chunk_samples() { return CHUNK_SAMPLES; }

EMSCRIPTEN_KEEPALIVE
double mdxweb_elapsed() { return E ? E->elapsed : 0.0; }

EMSCRIPTEN_KEEPALIVE
double mdxweb_duration() { return E ? E->duration : 0.0; }

EMSCRIPTEN_KEEPALIVE
int mdxweb_running() { return (E && E->running) ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
const char* mdxweb_title() { return E ? E->title : ""; }

EMSCRIPTEN_KEEPALIVE
int mdxweb_finished() {
  if (!E) return 1;
  return (MXDRVG_GetTerminated() || !E->running) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void mdxweb_shutdown() {
  if (!E) return;
  MXDRVG_End();
  // Restore the driver's own delegates before the wrappers that were installed
  // over them are destroyed, otherwise the driver would keep pointing at freed
  // objects and a later mdxweb_init() would crash.
  if (E->opm_original) {
    MXDRVG_SetOPMDelegate(E->opm_original);
    E->opm_original = nullptr;
  }
  if (E->pcm8_original) {
    MXDRVG_SetPCM8(E->pcm8_original);
    E->pcm8_original = nullptr;
  }
  if (E->visualizer) {
    E->visualizer->shutdown();
    delete E->visualizer;
    E->visualizer = nullptr;
  }
  delete E->opm_wrapper;
  E->opm_wrapper = nullptr;
  delete E->pcm8_wrapper;
  E->pcm8_wrapper = nullptr;
  delete E->ym_state;
  E->ym_state = nullptr;
  mdxweb_free_data();
  E->initialized = false;
}

EMSCRIPTEN_KEEPALIVE
void mdxweb_set_callbacks(const WebCallbacks* cb) {
  if (!cb) return;
  g_cb = *cb;
}

EMSCRIPTEN_KEEPALIVE
int mdxweb_audio_chunk_bytes() { return CHUNK_SAMPLES * 2 * sizeof(short); }

// Kept so the exported symbol list stays stable; the scratch buffer is sized in
// mdxweb_init().
EMSCRIPTEN_KEEPALIVE
int mdxweb_main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  return 0;
}

}  // extern "C"
