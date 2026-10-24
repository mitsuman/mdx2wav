#include "msm6258.h"

namespace zmd {
namespace {

// Same tables as the X68000 PCM8 emulation (gamdx/pcm8/global.h).
const int kStepTable[49] = {
    16,   17,   19,   21,   23,   25,   28,   31,   34,   37,   41,   45,
    50,   55,   60,   66,   73,   80,   88,   97,   107,  118,  130,  143,
    157,  173,  190,  209,  230,  253,  279,  307,  337,  371,  408,  449,
    494,  544,  598,  658,  724,  796,  876,  963,  1060, 1166, 1282, 1411,
    1552,
};

const int kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

inline int Clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

void DecodeMsm6258(const uint8_t* data, size_t bytes, std::vector<int16_t>* out) {
  out->clear();
  out->reserve(bytes * 2);
  int scale = 0;
  int pcm = 0;
  for (size_t i = 0; i < bytes; i++) {
    const uint8_t b = data[i];
    for (int half = 0; half < 2; half++) {
      const int nib = half == 0 ? (b & 0x0f) : ((b >> 4) & 0x0f);
      const int step = kStepTable[scale];
      int diff = step >> 3;
      if (nib & 4) diff += step;
      if (nib & 2) diff += step >> 1;
      if (nib & 1) diff += step >> 2;
      if (nib & 8) diff = -diff;
      pcm = Clamp(pcm + diff, -2047, 2047);
      scale = Clamp(scale + kIndexTable[nib], 0, 48);
      out->push_back((int16_t)(pcm * 16));
    }
  }
}

}  // namespace zmd
