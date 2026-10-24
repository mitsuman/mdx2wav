#ifndef MDX2WAV_LIBVGM_BACKEND_H
#define MDX2WAV_LIBVGM_BACKEND_H

#include <stdint.h>

#include <string>

// Thin wrapper around libvgm's player so the rest of the code does not have to
// include its headers (and their VGMPlayer/VGMPlayer name clash).
class LibVgmAudio {
 public:
  LibVgmAudio();
  ~LibVgmAudio();

  bool Load(const char* path, std::string* error);
  void Start(int loop_count, double fade_seconds);
  // Renders up to `frames` stereo frames at 44100 Hz.  Returns the number of
  // frames written (0 when the song has ended).
  int Render(int16_t* out, int frames);
  double TotalTime() const;
  // YM2612 state read from the chip libvgm is playing.
  bool ReadYm2612(int ch, int* fnum, int* block, int* alg, int* fb, int* panl,
                  int* panr, int* egout, int* kon) const;
  const int16_t* Ym2612Wave(int ch, uint32_t* pos) const;
  bool IsFinished() const { return finished_; }

 private:
  void* player_;   // PlayerA*
  void* engine_;   // VGMPlayer* (libvgm)
  void* loader_;   // DATA_LOADER*
  bool finished_;
};

#endif
