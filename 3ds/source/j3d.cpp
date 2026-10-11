#include "j3d.h"

#include <stdlib.h>
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

// ---------------------------------------------------------------------------------------------
// Mesh builder
// ---------------------------------------------------------------------------------------------

namespace {

struct PosFormat {
    uint32_t comps = 3;
    uint32_t type = 4;  // 0=u8 1=s8 2=u16 3=s16 4=f32
    float scale = 1.0f;
    uint32_t stride = 12;
    const uint8_t* base = nullptr;
    size_t bytes = 0;
};

struct MeshBuilder {
    J3dMesh* mesh;
    uint32_t capacity = 0;
    bool first = true;
};

static bool readPos(const PosFormat& f, uint32_t index, float* out) {
    uint64_t off = (uint64_t)index * f.stride;
    if (off + f.stride > f.bytes) {
        return false;
    }
    const uint8_t* p = f.base + off;
    out[2] = 0.0f;
    for (uint32_t k = 0; k < f.comps && k < 3; k++) {
        float v = 0.0f;
        switch (f.type) {
            case 0: v = (float)p[k] * f.scale; break;
            case 1: v = (float)(int8_t)p[k] * f.scale; break;
            case 2: v = (float)sdfs_be16(p + 2 * k) * f.scale; break;
            case 3: v = (float)(int16_t)sdfs_be16(p + 2 * k) * f.scale; break;
            default: {
                uint32_t bits = sdfs_be32(p + 4 * k);
                memcpy(&v, &bits, 4);
                break;
            }
        }
        out[k] = v;
    }
    return true;
}

static bool addTri(MeshBuilder& b, const float* a, const float* bb, const float* c) {
    J3dMesh* m = b.mesh;
    if (m->triCount >= 200000) {
        return false;  // safety cap
    }
    if (m->triCount == b.capacity) {
        uint32_t cap = b.capacity ? b.capacity * 2 : 256;
        float* grown = (float*)realloc(m->tris, (size_t)cap * 9 * sizeof(float));
        if (!grown) {
            return false;
        }
        m->tris = grown;
        b.capacity = cap;
    }
    float* t = m->tris + (size_t)m->triCount * 9;
    memcpy(t, a, 12);
    memcpy(t + 3, bb, 12);
    memcpy(t + 6, c, 12);
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) {
            float x = t[v * 3 + k];
            if (b.first || x < m->minv[k]) m->minv[k] = x;
            if (b.first || x > m->maxv[k]) m->maxv[k] = x;
        }
        b.first = false;
    }
    m->triCount++;
    return true;
}

static void triIdx(MeshBuilder& b, const PosFormat& f, uint32_t i0, uint32_t i1, uint32_t i2) {
    float a[3], bb[3], c[3];
    if (readPos(f, i0, a) && readPos(f, i1, bb) && readPos(f, i2, c)) {
        addTri(b, a, bb, c);
    }
}

}  // namespace

bool j3d_build_mesh(const uint8_t* d, size_t size, J3dMesh* out) {
    memset(out, 0, sizeof(*out));
    if (size < 0x20 || memcmp(d, "J3D2", 4) != 0) {
        return false;
    }
    uint32_t fileSize = sdfs_be32(d + 8);
    if (fileSize != 0 && fileSize < size) {
        size = fileSize;
    }
    const uint8_t* vtx = nullptr;
    const uint8_t* shp = nullptr;
    uint32_t vtxSize = 0, shpSize = 0;
    uint32_t chunkCount = sdfs_be32(d + 0x0C);
    uint64_t off = 0x20;
    for (uint32_t i = 0; i < chunkCount; i++) {
        if (!fits(size, off, 8)) {
            break;
        }
        const uint8_t* c = d + off;
        uint32_t csize = sdfs_be32(c + 4);
        if (csize < 8 || !fits(size, off, csize)) {
            break;
        }
        if (memcmp(c, "VTX1", 4) == 0) {
            vtx = c;
            vtxSize = csize;
        } else if (memcmp(c, "SHP1", 4) == 0) {
            shp = c;
            shpSize = csize;
        }
        off += csize;
    }
    if (!vtx || !shp || vtxSize < 0x4C || shpSize < 0x2C) {
        return false;
    }

    // --- positions: format + array
    PosFormat pf;
    bool havePos = false;
    uint32_t frac = 0;
    uint32_t fmtOff = sdfs_be32(vtx + 8);
    for (uint32_t e = fmtOff; fits(vtxSize, e, 16); e += 16) {
        uint32_t attr = sdfs_be32(vtx + e);
        if (attr == 0xFF) {
            break;
        }
        if (attr == 9) {
            pf.comps = sdfs_be32(vtx + e + 4) == 0 ? 2 : 3;
            pf.type = sdfs_be32(vtx + e + 8);
            frac = vtx[e + 12];
            havePos = true;
        }
    }
    uint32_t posOff = sdfs_be32(vtx + 0x0C);
    if (!havePos || posOff == 0 || posOff >= vtxSize || pf.type > 4) {
        return false;
    }
    static const uint32_t kTypeSize[5] = {1, 1, 2, 2, 4};
    pf.stride = pf.comps * kTypeSize[pf.type];
    pf.scale = (pf.type < 4 && frac < 31) ? 1.0f / (float)(1u << frac) : 1.0f;
    uint32_t next = vtxSize;
    for (int k = 0; k < 13; k++) {
        uint32_t v = sdfs_be32(vtx + 0x0C + 4 * k);
        if (v > posOff && v < next) {
            next = v;
        }
    }
    pf.base = vtx + posOff;
    pf.bytes = next - posOff;

    // --- shapes -> triangles
    uint32_t* idx = (uint32_t*)malloc(65536 * sizeof(uint32_t));
    if (!idx) {
        return false;
    }
    MeshBuilder mb;
    mb.mesh = out;

    uint32_t shapeCount = sdfs_be16(shp + 8);
    uint32_t shapeOff = sdfs_be32(shp + 0x0C);
    uint32_t attrBase = sdfs_be32(shp + 0x18);
    uint32_t drawBase = sdfs_be32(shp + 0x20);
    uint32_t packBase = sdfs_be32(shp + 0x28);

    for (uint32_t s = 0; s < shapeCount; s++) {
        uint64_t sp = (uint64_t)shapeOff + (uint64_t)s * 0x28;
        if (!fits(shpSize, sp, 0x28)) {
            break;
        }
        uint32_t groups = sdfs_be16(shp + sp + 2);
        uint32_t attrOff = sdfs_be16(shp + sp + 4);
        uint32_t firstGroup = sdfs_be16(shp + sp + 8);

        uint32_t vertBytes = 0, posByte = 0, posKind = 0;
        bool ok = true;
        for (uint64_t a = (uint64_t)attrBase + attrOff; fits(shpSize, a, 8); a += 8) {
            uint32_t attr = sdfs_be32(shp + a);
            uint32_t kind = sdfs_be32(shp + a + 4);
            if (attr == 0xFF) {
                break;
            }
            uint32_t sz = 0;
            if (kind == 1 && attr <= 8) {
                sz = 1;
            } else if (kind == 2) {
                sz = 1;
            } else if (kind == 3) {
                sz = 2;
            } else if (kind != 0) {
                ok = false;
            }
            if (attr == 9 && sz) {
                posByte = vertBytes;
                posKind = kind;
            }
            vertBytes += sz;
        }
        if (!ok || vertBytes == 0 || posKind < 2) {
            continue;
        }

        for (uint32_t g = 0; g < groups; g++) {
            uint64_t pk = (uint64_t)packBase + (uint64_t)(firstGroup + g) * 8;
            if (!fits(shpSize, pk, 8)) {
                break;
            }
            uint32_t psize = sdfs_be32(shp + pk);
            uint32_t poff = sdfs_be32(shp + pk + 4);
            uint64_t dl = (uint64_t)drawBase + poff;
            if (!fits(shpSize, dl, psize)) {
                continue;
            }
            uint32_t pos = 0;
            while (pos + 3 <= psize) {
                uint8_t cmd = shp[dl + pos];
                if (cmd == 0) {
                    break;
                }
                uint32_t n = sdfs_be16(shp + dl + pos + 1);
                pos += 3;
                if (!fits(psize, pos, (uint64_t)n * vertBytes)) {
                    break;
                }
                for (uint32_t v = 0; v < n; v++) {
                    const uint8_t* vp = shp + dl + pos + (size_t)v * vertBytes + posByte;
                    idx[v] = (posKind == 3) ? sdfs_be16(vp) : vp[0];
                }
                pos += n * vertBytes;

                switch (cmd & 0xF8) {
                    case 0x90:  // triangles
                        for (uint32_t i = 0; i + 2 < n; i += 3) {
                            triIdx(mb, pf, idx[i], idx[i + 1], idx[i + 2]);
                        }
                        break;
                    case 0x98:  // triangle strip
                        for (uint32_t i = 0; i + 2 < n; i++) {
                            if (i & 1) {
                                triIdx(mb, pf, idx[i + 1], idx[i], idx[i + 2]);
                            } else {
                                triIdx(mb, pf, idx[i], idx[i + 1], idx[i + 2]);
                            }
                        }
                        break;
                    case 0xA0:  // triangle fan
                        for (uint32_t i = 1; i + 1 < n; i++) {
                            triIdx(mb, pf, idx[0], idx[i], idx[i + 1]);
                        }
                        break;
                    case 0x80:  // quads
                        for (uint32_t i = 0; i + 3 < n; i += 4) {
                            triIdx(mb, pf, idx[i], idx[i + 1], idx[i + 2]);
                            triIdx(mb, pf, idx[i], idx[i + 2], idx[i + 3]);
                        }
                        break;
                    default: break;
                }
            }
        }
    }
    free(idx);
    if (out->triCount == 0) {
        j3d_free_mesh(out);
        return false;
    }
    return true;
}

void j3d_free_mesh(J3dMesh* m) {
    free(m->tris);
    memset(m, 0, sizeof(*m));
}
