// Country-Box launcher: a minimal PNG writer for the launcher's own screenshots (--shot-dir): RGBA rows, stored
// (uncompressed) deflate blocks, no dependency.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// pixels: h rows of w RGBA bytes, pitch bytes apart. 0 on success.
int png_write_rgba(const char *path, const uint8_t *pixels, int w, int h, int pitch);

#ifdef __cplusplus
}
#endif
