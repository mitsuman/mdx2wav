#include "libvgm_backend.h"

#include <string.h>

#include "../../third_party/libvgm/player/playera.hpp"
#include "../../third_party/libvgm/player/vgmplayer.hpp"
#include "../../third_party/libvgm/utils/FileLoader.h"

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

bool LibVgmAudio::Load(const char* path, std::string* error) {
  PlayerA* player = new PlayerA();
  VGMPlayer* engine = new VGMPlayer();
  player->RegisterPlayerEngine(engine);
  player->SetOutputSettings(44100, 2, 16, 4096);

  DATA_LOADER* loader = FileLoader_Init(path);
  if (!loader || DataLoader_Load(loader)) {
    if (error) *error = "Cannot open VGM file";
    delete player;
    delete engine;
    if (loader) DataLoader_Deinit(loader);
    return false;
  }
  if (player->LoadFile(loader)) {
    if (error) *error = "libvgm cannot play this VGM";
    delete player;
    delete engine;
    DataLoader_Deinit(loader);
    return false;
  }
  player_ = player;
  engine_ = engine;
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
  const UINT32 got = player->Render((UINT32)frames, out);
  if (got == 0) finished_ = true;
  return (int)got;
}

double LibVgmAudio::TotalTime() const {
  PlayerA* player = static_cast<PlayerA*>(player_);
  if (!player) return 0.0;
  return player->GetTotalTime(PLAYTIME_LOOP_INCL);
}
