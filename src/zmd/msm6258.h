// Minimal MSM6258 ADPCM decoder used by the Z-MUSIC port.
//
// The X68000's built-in ADPCM channel is an OKI MSM6258.  ZPD files store
// 4-bit MSM6258 data; Z-MUSIC converts it to 16-bit PCM when registering a
// sample.  This module performs the same 4-bit -> 16-bit conversion offline.

#ifndef MDX2WAV_MSM6258_H
#define MDX2WAV_MSM6258_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace zmd {

// Decodes `bytes` bytes of MSM6258 ADPCM (low nibble first) to 16-bit PCM.
// The decoder runs at the chip's nominal 15.6 kHz sample rate.
void DecodeMsm6258(const uint8_t* data, size_t bytes, std::vector<int16_t>* out);

}  // namespace zmd

#endif  // MDX2WAV_MSM6258_H
