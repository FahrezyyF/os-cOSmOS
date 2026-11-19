#ifndef _USER_INFLATE_H
#define _USER_INFLATE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * DEFLATE / zlib decoder (RFC 1950, RFC 1951): stored, fixed and dynamic Huffman blocks.
 * Used for big data files on the 4 MiB root file system (the DOOM WAD, the Bad Apple!!
 * video), which are stored as CZIP:
 *
 *   "CZIP"  u32 original size (little endian)  zlib stream (python3 zlib.compress)
 *
 * tools/czip.py makes them.
 */

#define CZIP_HEADER_SIZE 8

// Raw zlib stream -> dst. 0 ok, < 0 corrupt data / dst too small; *out_len = bytes written
int inflate_zlib(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

bool czip_is(const uint8_t *data, uint32_t len);
// Whole CZIP file -> malloc'd buffer (+1 zero byte). false when corrupt / out of memory
bool czip_unpack(const uint8_t *data, uint32_t len, uint8_t **out, uint32_t *out_len);

#endif
