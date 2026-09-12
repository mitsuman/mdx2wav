// Native unit test for the browser harness' MDX loading path.
//
// It drives the same mdxweb_load_data() function that the WebAssembly build
// uses, on the host, and writes the resulting PCM so web/tools/compare_pcm.py
// can compare it against the mdx2wav binary.
//
// Build: web/build_native_test.sh

#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "gamdx/mxdrvg/mxdrvg.h"
#include "web/src/mdxweb_load.h"

namespace {

const int SAMPLE_RATE = 44100;
const int MDX_BUF_SIZE = 256 * 1024;
const int PDX_BUF_SIZE = 1024 * 1024;
const int FILTER_MODE = 0;
const int CHUNK_SAMPLES = SAMPLE_RATE / 100;

bool ReadAll(const char* path, std::vector<unsigned char>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize((size_t)n);
  size_t got = fread(out->data(), 1, (size_t)n, f);
  fclose(f);
  return got == (size_t)n;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s song.mdx [pdx|-] [frames] [out.pcm]\n", argv[0]);
    return 2;
  }
  const char* mdx_path = argv[1];
  const char* pdx_path = (argc > 2 && argv[2][0] != '-') ? argv[2] : nullptr;
  const int frames = argc > 3 ? atoi(argv[3]) : SAMPLE_RATE * 3;
  const char* out_path = argc > 4 ? argv[4] : nullptr;

  std::vector<unsigned char> mdx, pdx;
  if (!ReadAll(mdx_path, &mdx)) {
    fprintf(stderr, "cannot read %s\n", mdx_path);
    return 1;
  }
  if (pdx_path && !ReadAll(pdx_path, &pdx)) {
    fprintf(stderr, "cannot read %s\n", pdx_path);
    return 1;
  }

  MXDRVG_SetEmulationType(MXDRVG_YM2151TYPE_FMGEN);
  if (MXDRVG_Start(SAMPLE_RATE, FILTER_MODE, MDX_BUF_SIZE, PDX_BUF_SIZE) != 0) {
    fprintf(stderr, "MXDRVG_Start failed\n");
    return 1;
  }
  MXDRVG_TotalVolume(256);

  char title[256];
  if (!mdxweb_load_data(mdx.data(), (int)mdx.size(),
                        pdx.empty() ? nullptr : pdx.data(), (int)pdx.size(),
                        title, sizeof(title))) {
    fprintf(stderr, "load failed\n");
    return 1;
  }
  fprintf(stderr, "title: %s\n", title);

  const unsigned long dur = MXDRVG_MeasurePlayTime(2, 0);
  MXDRVG_PlayAt(0, 2, 0);

  std::vector<short> pcm((size_t)frames * 2);
  int written = 0;
  while (written < frames) {
    int want = frames - written;
    if (want > CHUNK_SAMPLES) want = CHUNK_SAMPLES;
    int got = MXDRVG_GetPCM(pcm.data() + (size_t)written * 2, want);
    if (got <= 0) break;
    written += got;
    if (MXDRVG_GetTerminated()) break;
  }
  fprintf(stderr, "rendered %d frames\n", written);

  if (out_path) {
    FILE* f = fopen(out_path, "wb");
    if (!f) {
      fprintf(stderr, "cannot write %s\n", out_path);
      return 1;
    }
    fwrite(pcm.data(), 2, (size_t)written * 2, f);
    fclose(f);
  }

  mdxweb_free_data();
  MXDRVG_End();
  return written > 0 ? 0 : 1;
}
