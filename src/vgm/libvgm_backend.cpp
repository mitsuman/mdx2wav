#include "libvgm_backend.h"

#include <stdio.h>
#include <string.h>

#include "../../third_party/libvgm/player/playera.hpp"
#include "../../third_party/libvgm/player/vgmplayer.hpp"
#include "../../third_party/libvgm/utils/FileLoader.h"
#include "../../third_party/libvgm/emu/EmuCores.h"
#include "../../third_party/libvgm/emu/cores/ym3438.h"

// Expose the player's device list so the visualizer can read the very same
// YM2612 that produces the audio.
class VisVGMPlayer : public VGMPlayer {
 public:
  void* Ym2612Chip() const {
    for (size_t i = 0; i < _devices.size(); i++) {
      const CHIP_DEVICE& d = _devices[i];
      if (!d.base.defInf.dataPtr || !d.base.defInf.devDef) continue;
      if (d.base.defInf.devDef->coreID != FCC_NUKE) continue;
      return d.base.defInf.dataPtr;
    }
    return nullptr;
  }
};

LibVgmAudio::LibVgmAudio()
    : player_(nullptr), engine_(nullptr), loader_(nullptr), finished_(false) {}

LibVgmAudio::~LibVgmAudio() {
  PlayerA* player = static_cast<PlayerA*>(player_);
  VGMPlayer* engine = static_cast<VGMPlayer*>(engine_);
  DATA_LOADER* loader = static_cast<DATA_LOADER*>(loader_);
  if (player) {
    player->Stop();
    player->UnloadFile();
  }
  delete player;
  delete engine;
  if (loader) DataLoader_Deinit(loader);
}

bool LibVgmAudio::ReadYm2612(int ch, int* fnum, int* block, int* alg, int* fb,
                             int* panl, int* panr, int* egout, int* kon) const {
  VisVGMPlayer* engine = static_cast<VisVGMPlayer*>(const_cast<void*>(engine_));
  if (!engine) return false;
  void* chip = engine->Ym2612Chip();
  if (!chip) return false;
  nukedopn2_vis_state(chip, ch, fnum, block, alg, fb, panl, panr, egout, kon);
  return true;
}

const int16_t* LibVgmAudio::Ym2612Wave(int ch, uint32_t* pos) const {
  if (pos) *pos = nukedopn2_vis_pos(ch);
  return nukedopn2_vis_wave(ch);
}

bool LibVgmAudio::DacEnabled() const {
  return nukedopn2_vis_dac_enabled() != 0;
}

const int16_t* LibVgmAudio::DacWave(uint32_t* pos) const {
  if (pos) *pos = nukedopn2_vis_dac_pos();
  return nukedopn2_vis_dac();
}

bool LibVgmAudio::Load(const char* path, std::string* error) {
  PlayerA* player = static_cast<PlayerA*>(player_);
  if (!player) {
    player = new PlayerA();
    VisVGMPlayer* engine = new VisVGMPlayer();
    player->RegisterPlayerEngine(engine);
    player->SetOutputSettings(44100, 2, 16, 4096);
    player_ = player;
    engine_ = engine;
  } else {
    // Reuse the player: unloading and loading again with the same instance is
    // safe, while destroying a libvgm player after a song has been unloaded
    // crashes.
    player->UnloadFile();
    if (loader_) {
      DataLoader_Deinit(static_cast<DATA_LOADER*>(loader_));
      loader_ = nullptr;
    }
  }

  DATA_LOADER* loader = FileLoader_Init(path);
  if (!loader || DataLoader_Load(loader)) {
    if (error) *error = "Cannot open VGM file";
    if (loader) DataLoader_Deinit(loader);
    return false;
  }
  if (player->LoadFile(loader)) {
    if (error) *error = "libvgm cannot play this VGM";
    DataLoader_Deinit(loader);
    return false;
  }
  loader_ = loader;
  finished_ = false;
  return true;
}

void LibVgmAudio::Start(int loop_count, double fade_seconds) {
  PlayerA* player = static_cast<PlayerA*>(player_);
  if (!player) return;
  finished_ = false;
  player->SetLoopCount(loop_count > 0 ? (UINT32)loop_count : 1);
  player->SetFadeSamples(fade_seconds > 0.0 ? (UINT32)(fade_seconds * 44100.0) : 0);
  player->SetEndSilenceSamples(0);
  player->Start();
}

int LibVgmAudio::Render(int16_t* out, int frames) {
  PlayerA* player = static_cast<PlayerA*>(player_);
  if (!player || frames <= 0) return 0;
  // PlayerA works in bytes: 2 channels * 16 bit = 4 bytes per frame.
  const UINT32 got = player->Render((UINT32)frames * 4, out);
  if (got == 0) finished_ = true;
  return (int)(got / 4);
}

double LibVgmAudio::TotalTime() const {
  PlayerA* player = static_cast<PlayerA*>(player_);
  if (!player) return 0.0;
  return player->GetTotalTime(PLAYTIME_LOOP_INCL);
}
