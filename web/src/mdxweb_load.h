// MDX/PDX loading shared by the WebAssembly harness and the native test.
//
// This is the browser-side equivalent of LoadMDX() in src/mdx2wav.cpp: it takes
// the raw file contents (handed in from JavaScript) and prepares the buffers
// that MXDRVG_SetData() expects, including the 10 byte header the driver reads
// just *before* the body pointer.

#ifndef MDXWEB_LOAD_H
#define MDXWEB_LOAD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load MDX (and optional PDX) data into the driver.
// `title` receives the raw Shift_JIS title from the MDX file.
bool mdxweb_load_data(const unsigned char* mdx_raw, int mdx_raw_size,
                      const unsigned char* pdx_raw, int pdx_raw_size,
                      char* title, int title_len);

// Release the buffers allocated by mdxweb_load_data().
void mdxweb_free_data(void);

#ifdef __cplusplus
}
#endif

#endif  // MDXWEB_LOAD_H
