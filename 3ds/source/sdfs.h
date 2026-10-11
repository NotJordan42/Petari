// Minimal SD-card game file access for the 3DS port. Plain C++/newlib only (no Petari or MSL headers).
// Game files are the player's own extracted dump, expected at sdmc:/petari/files/<folders from the disc>.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Number of entries in a directory under the game root (e.g. "StageData"), or -1 if it can't be opened.
int sdfs_count(const char* relDir);

// Finds the first regular file under relDir (descends up to 3 levels). outRel is relative to the game root.
bool sdfs_find_first_file(const char* relDir, char* outRel, size_t outN, long* outSize);

// Reads the first n bytes of a game file (path relative to the game root). Returns false on failure.
bool sdfs_read_head(const char* rel, uint8_t* buf, size_t n);

// The game's data is big-endian (Wii/PowerPC); the 3DS CPU is little-endian. Always read multi-byte fields with these.
inline uint32_t sdfs_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint16_t sdfs_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
