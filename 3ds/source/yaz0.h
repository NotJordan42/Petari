// Yaz0 decompression (the compression used by most Wii/GameCube Nintendo archives, e.g. .arc inside StageData).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Uncompressed size from a Yaz0 header, or 0 if src is not Yaz0.
uint32_t yaz0_size(const uint8_t* src, size_t srcSize);

// Decodes a Yaz0 stream into dst (dstSize must be yaz0_size()). Returns false on truncated/corrupt input.
bool yaz0_decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize);
