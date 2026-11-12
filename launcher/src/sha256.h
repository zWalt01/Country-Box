// Country-Box launcher: SHA-256 of files. Uses the system's libcrypto when it can be loaded (SteamOS and desktop Linux
// ship libcrypto.so.3, whose SHA-256 uses the CPU's SHA instructions), else a portable implementation.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Sha256 Sha256;
Sha256 *sha256_new(void);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);   // also frees s

// Hashes a whole file into hex (65 bytes with the NUL). progress (may be NULL) gets the bytes read so far, added to
// atomically; *cancel (may be NULL) stops it. 0 on success, -1 when the file cannot be read or it was cancelled.
int sha256_file(const char *path, char hex[65], volatile int64_t *progress, volatile int *cancel);
// Which implementation runs ("libcrypto" or "portable"), for the log.
const char *sha256_backend(void);

#ifdef __cplusplus
}
#endif
