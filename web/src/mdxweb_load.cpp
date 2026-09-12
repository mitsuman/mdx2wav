// MDX/PDX loading shared by the WebAssembly harness and the native test.
//
// This is the browser-side equivalent of LoadMDX() in src/mdx2wav.cpp: it takes
// the raw file contents (handed in from JavaScript) and prepares the buffers
// that MXDRVG_SetData() expects.
//
// MXDRVG is handed a pointer to the file contents and reads a 10 byte driver
// header at the ten bytes just before it.  The native loader gets that layout
// for free because it allocates `size + 10` bytes, reads the file at offset 10
// and writes the header into the preceding bytes.  Here the file contents are
// placed at an aligned offset with the header sitting immediately before them.

#include "mdxweb_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamdx/mxdrvg/mxdrvg.h"

namespace {

typedef unsigned char u8;

const int MAGIC_OFFSET = 10;
// Extra room past the end of each buffer so the driver's copy routine cannot
// run off the end of the allocation (the native build gets this for free from
// the malloc rounding).
const int SLACK = 64 * 1024;

u8* g_mdx_base = nullptr;  // allocation start (only used to free)
u8* g_mdx = nullptr;       // driver header; file contents at +MAGIC_OFFSET
u8* g_pdx = nullptr;
int g_pdx_size = 0;
bool g_verbose = false;

}  // namespace

bool mdxweb_load_data(const unsigned char* mdx_raw, int mdx_raw_size,
                      const unsigned char* pdx_raw, int pdx_raw_size,
                      char* title, int title_len) {
  if (!mdx_raw || mdx_raw_size <= MAGIC_OFFSET) {
    return false;
  }

  mdxweb_free_data();

  // ------------------------------------------------------------------ PDX
  // Driver header at [0..9], sample data from [MAGIC_OFFSET].  MXDRVG is handed
  // the header pointer and reads the data right after it.
  if (pdx_raw && pdx_raw_size > 0) {
    g_pdx = (u8*)calloc(1, (size_t)pdx_raw_size + MAGIC_OFFSET + SLACK);
    if (!g_pdx) return false;
    memcpy(g_pdx + MAGIC_OFFSET, pdx_raw, pdx_raw_size);
    g_pdx_size = pdx_raw_size + MAGIC_OFFSET;

    g_pdx[0] = 0x00;
    g_pdx[1] = 0x00;
    g_pdx[2] = 0x00;
    g_pdx[3] = 0x00;
    g_pdx[4] = 0x00;
    g_pdx[5] = 0x0a;
    g_pdx[6] = 0x00;
    g_pdx[7] = 0x02;
    g_pdx[8] = 0x00;
    g_pdx[9] = 0x00;
  }

  // ------------------------------------------------------------------ MDX
  // Layout: [pad][10 byte driver header][file contents][slack].
  //
  // MXDRVG is handed the pointer to the header: it reads the header from
  // ptr[0..9] and the song body from ptr[10..].  The body starts at file[pos],
  // so the pointer passed is file + pos - 10.  Everything the driver walks with
  // 32 bit loads has to be 4 byte aligned, so the contents are placed at an
  // aligned offset and the pointer is aligned by construction.
  const int mdx_size = mdx_raw_size + MAGIC_OFFSET;
  const int kHeadroom = 34;  // makes the body pointer 4 byte aligned
  u8* base = (u8*)calloc(1, (size_t)mdx_size + kHeadroom + MAGIC_OFFSET + SLACK);
  if (!base) {
    mdxweb_free_data();
    return false;
  }
  g_mdx_base = base;
  u8* file = base + kHeadroom;        // file contents
  memcpy(file, mdx_raw, mdx_raw_size);

  // Skip title.
  int pos = 0;
  if (title && title_len > 0) {
    char* ptitle = title;
    while (pos < mdx_raw_size && --title_len > 0) {
      *ptitle++ = file[pos];
      if (file[pos] == 0x0d && file[pos + 1] == 0x0a) break;
      pos++;
    }
    *ptitle = 0;
  }

  while (pos < mdx_raw_size) {
    u8 c = file[pos++];
    if (c == 0x1a) break;
  }

  char* pdx_name = (char*)file + pos;

  while (pos < mdx_raw_size) {
    u8 c = file[pos++];
    if (c == 0) break;
  }

  if (pos >= mdx_raw_size) {
    mdxweb_free_data();
    return false;
  }

  // The PDX contents are supplied by the caller, so only the driver's own
  // ".pdx" suffix handling has to be mirrored here.
  if (*pdx_name) {
    int pdx_name_len = (int)strlen(pdx_name);
    if (pdx_name_len > 4 && pdx_name[pdx_name_len - 4] == '.') {
      pdx_name[pdx_name_len - 4] = 0;
    }
  }

  // Driver header.
  //
  // The native loader writes the header at mdx_body_pos - MAGIC_OFFSET inside
  // its buffer, which places it ten bytes before the song body, and passes that
  // pointer to MXDRVG_SetData().  Here the body is at file[pos], so the header
  // goes at (file + pos) - MAGIC_OFFSET and the same pointer is handed over.
  // `pos` is chosen so this pointer stays 4 byte aligned.
  u8* const drive_ptr = file + (pos - MAGIC_OFFSET);
  drive_ptr[0] = 0x00;
  drive_ptr[1] = 0x00;
  drive_ptr[2] = (g_pdx ? 0x00 : 0xff);
  drive_ptr[3] = (g_pdx ? 0x00 : 0xff);
  drive_ptr[4] = 0;
  drive_ptr[5] = 0x0a;
  drive_ptr[6] = 0x00;
  drive_ptr[7] = 0x08;
  drive_ptr[8] = 0x00;
  drive_ptr[9] = 0x00;

  if (g_verbose) {
    fprintf(stderr, "mdx body pos  :0x%x\n", pos);
    fprintf(stderr, "mdx body size :0x%x\n", mdx_size - pos);
  }

  MXDRVG_SetData(drive_ptr, mdx_size, g_pdx, g_pdx_size);
  return true;
}

void mdxweb_free_data(void) {
  free(g_mdx_base);
  g_mdx_base = nullptr;
  g_mdx = nullptr;
  free(g_pdx);
  g_pdx = nullptr;
  g_pdx_size = 0;
}
