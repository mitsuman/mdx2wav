#ifndef PSF_PLAYER_H
#define PSF_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "../spc/spc_resampler.h"

// Plays a PlayStation .psf/.minipsf file through the PS1 (R3000 + SPU) core and
// exposes the live SPU state for the visualizer.
//
// PSF files contain a PS-X EXE plus optional sample data shared through a
// .psflib.  Playing them requires a PlayStation BIOS image; the caller has to
// supply one (see SetBiosPath), because a BIOS cannot be bundled with mdx2wav.
class PSFPlayer {
 public:
  static const int kVoices = 24;
  enum { kWaveformSamples = 1024 };

  struct VoiceState {
    bool active;          // envelope is not fully released
    int pitch;            // 0x0000-0x3FFF
    int volume_left;      // 0x0000-0x3FFF
    int volume_right;
    int envelope;         // current envelope level (0-0x7FFF)
    int adsr1;
    int adsr2;
    int start_address;    // SPU RAM address / 8
    int loop_address;
    int current_address;
    int16_t waveform[kWaveformSamples];
    int waveform_samples;
  };

  struct GlobalState {
    int master_volume_left;
    int master_volume_right;
    int reverb_enabled;
    int main_enabled;
    int key_on_mask;      // bit per voice for the most recent KON? (may be 0)
  };

  PSFPlayer();
  ~PSFPlayer();

  // Uses psflib to read the file (and any .psflib next to it).
  bool Load(const char* path, const char* bios_path, std::string* error);

  void SetSampleRate(int rate);
  void Start(int loop, double max_duration, double fadeout_seconds);

  int Generate(int16_t* out, int frames);

  bool IsLoaded() const { return loaded_; }
  bool IsFinished() const { return finished_; }
  double Duration() const { return duration_; }
  double Elapsed() const { return elapsed_; }
  int SampleRate() const { return sample_rate_; }

  const char* Title() const { return title_.c_str(); }
  const char* Game() const { return game_.c_str(); }
  const char* Artist() const { return artist_.c_str(); }

  // Snapshot of the SPU state for the visualizer.
  void GetVoiceStates(VoiceState out[kVoices]) const;
  void GetGlobalState(GlobalState* out) const;

  static bool IsPSFFile(const uint8_t* data, size_t size);

  // Path candidates for the PS1 BIOS image.
  static std::string FindBios(const char* explicit_path);

  // Used by the psflib load callback.
  void AddExecutable(const uint8_t* data, size_t size);

 private:
  bool LoadBios(const char* path, std::string* error);
  void ResetCore();
  int GenerateCore(int16_t* out, int frames);

  std::vector<std::vector<uint8_t> > exes_;  // PS-X EXE chunks, libs first
  std::vector<uint8_t> bios_;
  void* state_;
  bool loaded_;
  bool finished_;
  int sample_rate_;
  int loop_;
  double max_duration_;
  double duration_;
  double fadeout_seconds_;
  double elapsed_;
  double tag_seconds_;
  double tag_fade_;

  std::string title_;
  std::string game_;
  std::string artist_;
  std::string path_;

  SPCResampler resampler_;
  std::vector<int16_t> raw_;
};

#endif  // PSF_PLAYER_H
