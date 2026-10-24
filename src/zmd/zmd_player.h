// Z-MUSIC (ZMD) player -- C++ port of the Z-MUSIC SYSTEM Ver.2 playback
// engine, limited to the built-in FM (YM2151) and MSM6258 ADPCM parts.
//
// The original Z-MUSIC SYSTEM was written by Zenji Nishikawa (西川善司).
// The source used as reference is https://github.com/kg68k/zmusic2
// (unified/zmusic.s, unified/interrupt.s, unified/work.s).
//
// This port implements the commands used by the Metal Unit II ZMD files:
// notes/rests, tempo, voice change, volume, pan, detune, repeats, DO/LOOP and
// the software pitch/amplitude LFO.  It also plays ZPD MSM6258 ADPCM samples
// on channel 8 ($40/$9B/$A2/$A9/$B7/$B8).

#ifndef MDX2WAV_ZMD_PLAYER_H
#define MDX2WAV_ZMD_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace FM {
class OPM;
}

class OPM_Delegate;

class ZmdPlayer {
 public:
  ZmdPlayer();
  ~ZmdPlayer();

  bool LoadFile(const char* path, std::string* error);
  bool Load(const uint8_t* data, size_t size, std::string* error);

  void SetSampleRate(int sample_rate);
  void SetLoopCount(int loops);   // number of DO/LOOP passes, 0 = unlimited
  void Start();

  // Renders interleaved stereo S16.  Returns the number of frames written.
  // Returns 0 once the song (and its release tail) has finished.
  int Generate(int16_t* out, int frames);

  bool IsFinished() const;
  bool HasAdpcm() const { return has_adpcm_; }
  bool AdpcmReady() const { return adpcm_ready_; }
  const std::string& AdpcmWarning() const { return adpcm_warning_; }
  const std::string& Title() const { return title_; }
  const std::string& LastError() const { return error_; }

  // Highest FM channel used by the track table (1..8, 0 when there is none).
  // The visualizer uses this to size its window like it does for MDX.
  int UsedFmChannels() const;

  // The YM2151 register stream can be mirrored into the visualizer before it
  // reaches the chip.  When no sink is set, registers go straight to the OPM.
  void SetRegisterSink(OPM_Delegate* sink) { register_sink_ = sink; }
  void WriteOpmDirect(uint8_t addr, uint8_t data);
  void* OpmPointer() const { return opm_; }

  // Pauses the sequencer while the OPM keeps mixing (the visualizer's X key).
  void SetPaused(bool paused) { paused_ = paused; }

  // Snapshot of the MSM6258 voice for the ADPCM row of the visualizer.
  struct AdpcmVisualState {
    bool active;
    bool sample_valid;
    int note;
    int rate;          // source rate in Hz after pitch/volume settings
    int volume;        // registration volume (100 = original)
    int pan;           // 0 mute, 1 left, 2 right, 3 centre
    const int16_t* pcm;
    int pcm_frames;
    double position;   // current position in source frames
    double step;       // source frames per output frame
    unsigned int keyons;
    AdpcmVisualState()
        : active(false), sample_valid(false), note(-1), rate(0), volume(100),
          pan(3), pcm(nullptr), pcm_frames(0), position(0.0), step(1.0),
          keyons(0) {}
  };
  bool GetAdpcmVisualState(AdpcmVisualState* out) const;
  // Fills up to max_samples output-rate samples ending at the current
  // playback position.  Returns the number of samples written.
  int GetAdpcmWaveform(int16_t* out, int max_samples) const;

  // Runs the sequencer without mixing audio and returns the length in
  // seconds.  The player is re-armed by this call; call Start() afterwards.
  double MeasureSeconds(double max_seconds = 3600.0);

 private:
  struct Voice {
    uint8_t data[55];
    Voice() { for (int i = 0; i < 55; i++) data[i] = 0; }
  };

  struct AdpcmSample {
    std::vector<int16_t> pcm;  // 15.6 kHz mono
    int pitch;                 // 0..24, 12 = original
    int volume;                // 1..300, 100 = original
    bool valid;
    AdpcmSample() : pitch(12), volume(100), valid(false) {}
  };

  struct AdpcmPlay {
    bool active;
    int note;
    double pos;
    double step;
    AdpcmPlay() : active(false), note(-1), pos(0.0), step(1.0) {}
  };

  struct Repeat {
    int count;
    int iter;
  };

  struct Lfo {
    bool on;
    int depth;
    int speed;      // Z-MUSIC p_*mod_spd (D6 value * 2)
    int wave;       // 0=saw, 1=square, 2=triangle, 3=saw2
    int delay;      // ticks
    int wait;
    double phase;
    double inc;
    int last_value;
    Lfo()
        : on(false), depth(0), speed(6), wave(2), delay(0), wait(0),
          phase(0.0), inc(0.25), last_value(0) {}
  };

  struct Track {
    int channel;
    size_t start;
    size_t pc;
    bool dead;

    int step_wait;
    int gate_wait;
    int note;          // MIDI note currently keyed on
    bool key_on;

    int volume;        // Z-MUSIC attenuated volume (127 = silent)
    int pan;           // 0=mute, 1=left, 2=right, 3=center
    int pgm;           // voice index (0-based)
    int detune_f;      // fine detune, 1 semitone = 64
    int detune_m;

    bool has_do;
    size_t do_ptr;
    int loop_pass;

    std::vector<Repeat> repeats;

    bool has_voice;
    uint8_t vdata[55];
    uint8_t carrier;   // bit0=OP1 bit1=OP2 bit2=OP3 bit3=OP4
    uint8_t af;        // feedback/algorithm
    uint8_t om;        // operator mask << 3
    int ol[4];         // operator TL from the voice

    Lfo pmod;
    Lfo amod;
    int amod_offset;

    Track()
        : channel(0), start(0), pc(0), dead(false), step_wait(0),
          gate_wait(0), note(-1), key_on(false), volume(0x15), pan(3),
          pgm(0), detune_f(0), detune_m(0), has_do(false), do_ptr(0),
          loop_pass(0), has_voice(false), carrier(0x08), af(0), om(0),
          amod_offset(0) {
      for (int i = 0; i < 4; i++) ol[i] = 0;
      for (int i = 0; i < 55; i++) vdata[i] = 0;
    }
  };

  bool Parse(std::string* error);
  bool LoadZpd(const std::string& filename, int dest_note, int pitch,
               int volume, int cut_offset, int cut_size, bool reverse);
  void StartAdpcm(int note);
  void StopAdpcm();
  void MixAdpcm(int16_t* out, int frames);
  void ResetTracks();
  void WriteVoice(Track& t, int pgm, bool preserve_pan);
  void ApplyPan(Track& t);
  void ApplyVolume(Track& t);
  void SetPitch(Track& t, int note, int kf_offset);
  void KeyOn(Track& t, int note);
  void KeyOff(Track& t);
  void EndTrack(Track& t);

  void Tick();
  void TickTrack(Track& t);
  void RunCommands(Track& t);
  void HandleNote(Track& t, int note, int step, int gate);
  void HandleRest(Track& t, int step, int gate);
  void HandleTempo(int bpm);
  void UpdateLfo(Track& t, Lfo& lfo, bool pitch);
  void ResetLfo(Track& t, Lfo& lfo);

  void RecalcPitchLfo(Track& t);
  void RecalcAmpLfo(Track& t);
  double TickSeconds() const;
  bool AllDead() const;
  uint16_t Be16(size_t pos) const;
  uint32_t Be32(size_t pos) const;
  void WriteReg(uint8_t addr, uint8_t data);

  std::vector<uint8_t> data_;
  std::vector<Voice> voices_;
  std::vector<Track> tracks_;
  std::vector<AdpcmSample> adpcm_;
  AdpcmPlay adpcm_play_;
  int adpcm_freq_;    // 0..4 -> 3.9/5.2/7.8/10.4/15.6 kHz
  int adpcm_pan_;     // 0 mute, 1 left, 2 right, 3 center
  int adpcm_track_volume_;  // 0..127 attenuation from the ADPCM track
  bool adpcm_ready_;
  std::string adpcm_warning_;
  std::string base_dir_;

  std::string title_;
  std::string error_;
  bool has_adpcm_;

  int sample_rate_;
  int loop_limit_;
  int tempo_;
  int mclk_;

  bool started_;
  bool all_dead_;
  bool tail_started_;
  int tail_remaining_;
  bool paused_;

  double tick_accum_;
  double samples_per_tick_;
  double total_seconds_;

  unsigned int adpcm_keyons_;

  OPM_Delegate* register_sink_;
  FM::OPM* opm_;
};

#endif  // MDX2WAV_ZMD_PLAYER_H
