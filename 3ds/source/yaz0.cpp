#include "yaz0.h"

#include <string.h>

#include "sdfs.h"

uint32_t yaz0_size(const uint8_t* src, size_t srcSize) {
    if (srcSize < 16 || memcmp(src, "Yaz0", 4) != 0) {
        return 0;
    }
    return sdfs_be32(src + 4);
}

bool yaz0_decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    if (srcSize < 16 || memcmp(src, "Yaz0", 4) != 0) {
        return false;
    }
    size_t s = 16;
    size_t d = 0;
    while (d < dstSize) {
        if (s >= srcSize) {
            return false;
        }
        uint8_t code = src[s++];
        for (int bit = 0; bit < 8 && d < dstSize; bit++, code = (uint8_t)(code << 1)) {
            if (code & 0x80) {  // literal byte
                if (s >= srcSize) {
                    return false;
                }
                dst[d++] = src[s++];
            } else {  // back-reference
                if (s + 2 > srcSize) {
                    return false;
                }
                uint8_t b1 = src[s++];
                uint8_t b2 = src[s++];
                size_t dist = (((size_t)(b1 & 0x0F) << 8) | b2) + 1;
                size_t n = b1 >> 4;
                if (n == 0) {
                    if (s >= srcSize) {
                        return false;
                    }
                    n = (size_t)src[s++] + 0x12;
                } else {
                    n += 2;
                }
                if (dist > d) {
                    return false;
                }
                if (n > dstSize - d) {
                    n = dstSize - d;
                }
                for (size_t i = 0; i < n; i++) {
                    dst[d] = dst[d - dist];
                    d++;
                }
            }
        }
    }
    return true;
}
