#ifndef SPC_VISUALIZER_H
#define SPC_VISUALIZER_H

class SPCPlayer;
class YM2151State;

// Copy the current S-DSP state into the visualizer's channel model.  This is
// the bridge between the SPC player and the YM2151-oriented Visualizer: the
// visualizer reuses its keyboard, waveform and spectrum code, while the SPC
// layout supplies different labels and per-voice information.
void UpdateSPCVisualizerState(SPCPlayer& player, YM2151State* state);

#endif  // SPC_VISUALIZER_H
