#include "spc_visualizer.h"

#include "spc_player.h"
#include "../visualizer/ym2151_state.h"

void UpdateSPCVisualizerState(SPCPlayer& player, YM2151State* state) {
  if (!state || !player.IsLoaded()) return;

  SPCPlayer::VoiceState voices[SPCPlayer::kVoices];
  player.GetVoiceStates(voices);

  for (int ch = 0; ch < SPCPlayer::kVoices; ch++) {
    const SPCPlayer::VoiceState& v = voices[ch];
    state->updateSPCVoice(
        ch, v.key_on, v.kon, v.note, v.pitch_note, v.volume, v.left_volume, v.right_volume,
        v.pitch, v.envelope, v.envelope_phase, v.source_address,
        v.loop_address, v.brr_address, v.sample_end_address,
        v.waveform, v.waveform_samples);
  }

  SPCPlayer::GlobalState global;
  player.GetGlobalState(&global);
  YM2151State::SPCGlobal out;
  out.master_volume_left = global.master_volume_left;
  out.master_volume_right = global.master_volume_right;
  out.echo_volume_left = global.echo_volume_left;
  out.echo_volume_right = global.echo_volume_right;
  out.echo_feedback = global.echo_feedback;
  out.echo_delay = global.echo_delay;
  out.echo_start = global.echo_start;
  out.echo_offset = global.echo_offset;
  out.echo_length = global.echo_length;
  out.noise_enable = global.noise_enable;
  out.key_on_mask = global.key_on_mask;
  out.directory_address = global.directory_address;
  out.cpu_pc = global.cpu_pc;
  state->setSPCGlobal(out);
}
