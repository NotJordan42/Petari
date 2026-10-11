#include "j3d.h"

#include <string.h>

#include "sdfs.h"

static inline bool fits(uint64_t limit, uint64_t off, uint64_t len) {
    return off + len <= limit;
}

static void parseInf(const uint8_t* c, uint32_t csize, J3dInfo* o) {
    if (csize >= 0x18) {
        o->infVertexCount = sdfs_be32(c + 0x10);
    }
}

static void parseVtx(const uint8_t* c, uint32_t csize, J3dInfo* o) {
    if (csize < 0x4C) {
        return;
    }
    uint32_t fmtOff = sdfs_be32(c + 8);
    uint32_t cnt = 0, typ = 0;
    bool havePos = false;
    for (uint32_t e = fmtOff; fits(csize, e, 16); e += 16) {
        uint32_t attr = sdfs_be32(c + e);
        if (attr == 0xFF) {  // GX_VA_NULL terminates the list
            break;
        }
        if (attr == 9) {  // GX_VA_POS
            cnt = sdfs_be32(c + e + 4);
            typ = sdfs_be32(c + e + 8);
            havePos = true;
        }
    }
    uint32_t posOff = sdfs_be32(c + 0x0C);  // first of 13 array offsets: pos, nrm, nbt, clr0-1, tex0-7
    if (!havePos || posOff == 0 || posOff >= csize || typ > 4) {
        return;
    }
    uint32_t next = csize;  // the array runs until the next array (or the end of the chunk)
    for (int k = 0; k < 13; k++) {
        uint32_t v = sdfs_be32(c + 0x0C + 4 * k);
        if (v > posOff && v < next) {
            next = v;
        }
    }
    static const uint32_t kTypeSize[5] = {1, 1, 2, 2, 4};
    uint32_t comps = (cnt == 0) ? 2 : 3;
    o->posComps = comps;
    o->posType = typ;
    o->posCount = (next - posOff) / (comps * kTypeSize[typ]);
}

static void parseShp(const uint8_t* c, uint32_t csize, J3dInfo* o) {
    if (csize < 0x2C) {
        return;
    }
    uint32_t shapeCount = sdfs_be16(c + 8);
    uint32_t shapeOff = sdfs_be32(c + 0x0C);
    uint32_t attrBase = sdfs_be32(c + 0x18);
    uint32_t drawBase = sdfs_be32(c + 0x20);
    uint32_t packBase = sdfs_be32(c + 0x28);
    o->shapeCount = shapeCount;

    for (uint32_t s = 0; s < shapeCount; s++) {
        uint64_t sp = (uint64_t)shapeOff + (uint64_t)s * 0x28;
        if (!fits(csize, sp, 0x28)) {
            break;
        }
        uint32_t groups = sdfs_be16(c + sp + 2);
        uint32_t attrOff = sdfs_be16(c + sp + 4);
        uint32_t firstGroup = sdfs_be16(c + sp + 8);

        // Vertex layout of this shape: a list of (attribute, how it is stored), ended by attribute 0xFF.
        uint32_t vertBytes = 0;
        bool ok = true;
        for (uint64_t a = (uint64_t)attrBase + attrOff; fits(csize, a, 8); a += 8) {
            uint32_t attr = sdfs_be32(c + a);
            uint32_t kind = sdfs_be32(c + a + 4);  // 0 none, 1 direct, 2 index8, 3 index16
            if (attr == 0xFF) {
                break;
            }
            if (kind == 1 && attr <= 8) {
                vertBytes += 1;  // matrix index, stored directly as one byte
            } else if (kind == 2) {
                vertBytes += 1;
            } else if (kind == 3) {
                vertBytes += 2;
            } else if (kind != 0) {
                ok = false;  // direct-stored vertex data: not handled yet
            }
        }
        if (!ok || vertBytes == 0) {
            o->unsupportedAttr = true;
            continue;
        }

        for (uint32_t g = 0; g < groups; g++) {
            uint64_t pk = (uint64_t)packBase + (uint64_t)(firstGroup + g) * 8;
            if (!fits(csize, pk, 8)) {
                break;
            }
            uint32_t psize = sdfs_be32(c + pk);
            uint32_t poff = sdfs_be32(c + pk + 4);
            uint64_t dl = (uint64_t)drawBase + poff;
            if (!fits(csize, dl, psize)) {
                continue;
            }
            o->packets++;
            uint32_t pos = 0;
            while (pos + 3 <= psize) {
                uint8_t cmd = c[dl + pos];
                if (cmd == 0) {  // padding
                    break;
                }
                uint32_t n = sdfs_be16(c + dl + pos + 1);
                pos += 3;
                if (!fits(psize, pos, (uint64_t)n * vertBytes)) {
                    break;
                }
                pos += n * vertBytes;
                switch (cmd & 0xF8) {
                    case 0x90: o->triangles += n / 3; break;              // triangles
                    case 0x98: if (n >= 3) o->triangles += n - 2; break;  // triangle strip
                    case 0xA0: if (n >= 3) o->triangles += n - 2; break;  // triangle fan
                    case 0x80: o->triangles += (n / 4) * 2; break;        // quads
                    default: break;                                       // lines / points
                }
            }
        }
    }
}

bool j3d_inspect(const uint8_t* d, size_t size, J3dInfo* o) {
    memset(o, 0, sizeof(*o));
    if (size < 0x20 || memcmp(d, "J3D2", 4) != 0) {
        return false;
    }
    memcpy(o->type, d + 4, 4);
    o->type[4] = 0;
    uint32_t fileSize = sdfs_be32(d + 8);
    if (fileSize != 0 && fileSize < size) {
        size = fileSize;
    }
    o->chunkCount = sdfs_be32(d + 0x0C);

    uint64_t off = 0x20;
    for (uint32_t i = 0; i < o->chunkCount; i++) {
        if (!fits(size, off, 8)) {
            break;
        }
        const uint8_t* c = d + off;
        uint32_t csize = sdfs_be32(c + 4);
        if (csize < 8 || !fits(size, off, csize)) {
            break;
        }
        if (memcmp(c, "INF1", 4) == 0) {
            parseInf(c, csize, o);
        } else if (memcmp(c, "VTX1", 4) == 0) {
            parseVtx(c, csize, o);
        } else if (memcmp(c, "SHP1", 4) == 0) {
            parseShp(c, csize, o);
        } else if (memcmp(c, "MAT3", 4) == 0 && csize >= 10) {
            o->materialCount = sdfs_be16(c + 8);
        } else if (memcmp(c, "TEX1", 4) == 0 && csize >= 10) {
            o->textureCount = sdfs_be16(c + 8);
        }
        o->chunksParsed++;
        off += csize;
    }
    return true;
}
