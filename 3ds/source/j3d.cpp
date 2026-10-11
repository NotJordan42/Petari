#include "j3d.h"

#include <math.h>
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

struct ArrFmt {
    uint32_t comps = 3;
    uint32_t type = 4;  // 0=u8 1=s8 2=u16 3=s16 4=f32
    float scale = 1.0f;
    uint32_t stride = 12;
    const uint8_t* base = nullptr;
    size_t bytes = 0;
    bool valid = false;
};

static const uint32_t kTypeSize[5] = {1, 1, 2, 2, 4};

// Reads the format entry for `attr` and the array location for header slot `slot` (0x0C + 4*slot) of a VTX1 chunk.
static ArrFmt readArray(const uint8_t* vtx, uint32_t vtxSize, uint32_t attr, uint32_t slot, uint32_t compsIfZero,
                        uint32_t compsIfNonZero) {
    ArrFmt f;
    uint32_t frac = 0;
    bool have = false;
    uint32_t fmtOff = sdfs_be32(vtx + 8);
    for (uint32_t e = fmtOff; fits(vtxSize, e, 16); e += 16) {
        uint32_t a = sdfs_be32(vtx + e);
        if (a == 0xFF) {
            break;
        }
        if (a == attr) {
            f.comps = sdfs_be32(vtx + e + 4) == 0 ? compsIfZero : compsIfNonZero;
            f.type = sdfs_be32(vtx + e + 8);
            frac = vtx[e + 12];
            have = true;
        }
    }
    uint32_t off = sdfs_be32(vtx + 0x0C + 4 * slot);
    if (!have || off == 0 || off >= vtxSize || f.type > 4) {
        return f;
    }
    f.stride = f.comps * kTypeSize[f.type];
    f.scale = (f.type < 4 && frac < 31) ? 1.0f / (float)(1u << frac) : 1.0f;
    uint32_t next = vtxSize;
    for (int k = 0; k < 13; k++) {
        uint32_t v = sdfs_be32(vtx + 0x0C + 4 * k);
        if (v > off && v < next) {
            next = v;
        }
    }
    f.base = vtx + off;
    f.bytes = next - off;
    f.valid = true;
    return f;
}

static bool readElem(const ArrFmt& f, uint32_t index, float* out) {
    uint64_t off = (uint64_t)index * f.stride;
    if (!f.valid || off + f.stride > f.bytes) {
        return false;
    }
    const uint8_t* p = f.base + off;
    out[0] = out[1] = out[2] = 0.0f;
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

struct MeshBuilder {
    J3dMesh* mesh;
    uint32_t capacity = 0;
    bool first = true;
};

static bool addTri(MeshBuilder& b, const float* p, const float* uv, int16_t tex) {
    J3dMesh* m = b.mesh;
    if (m->triCount >= 200000) {
        return false;  // safety cap
    }
    if (m->triCount == b.capacity) {
        uint32_t cap = b.capacity ? b.capacity * 2 : 256;
        float* t = (float*)realloc(m->tris, (size_t)cap * 9 * sizeof(float));
        if (!t) return false;
        m->tris = t;
        float* u = (float*)realloc(m->uvs, (size_t)cap * 6 * sizeof(float));
        if (!u) return false;
        m->uvs = u;
        int16_t* x = (int16_t*)realloc(m->triTex, (size_t)cap * sizeof(int16_t));
        if (!x) return false;
        m->triTex = x;
        b.capacity = cap;
    }
    memcpy(m->tris + (size_t)m->triCount * 9, p, 9 * sizeof(float));
    memcpy(m->uvs + (size_t)m->triCount * 6, uv, 6 * sizeof(float));
    m->triTex[m->triCount] = tex;
    for (int v = 0; v < 3; v++) {
        for (int k = 0; k < 3; k++) {
            float x = p[v * 3 + k];
            if (b.first || x < m->minv[k]) m->minv[k] = x;
            if (b.first || x > m->maxv[k]) m->maxv[k] = x;
        }
        b.first = false;
    }
    m->triCount++;
    return true;
}

// ---- joints / skinning matrices ---------------------------------------------------------------

struct Mat {
    float m[3][4];
};

static Mat matIdentity() {
    Mat r;
    memset(&r, 0, sizeof(r));
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
    return r;
}

static Mat matMul(const Mat& a, const Mat& b) {  // a * b, both treated as 4x4 with a last row of 0 0 0 1
    Mat r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.0f);
        }
    }
    return r;
}

static float readF(const uint8_t* p) {
    uint32_t b = sdfs_be32(p);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

// T * Rz * Ry * Rx * S
static Mat matFromSRT(const float* s, const float* r, const float* t) {
    float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]), sz = sinf(r[2]);
    float R[3][3] = {{cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz},
                     {cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz},
                     {-sy, sx * cy, cx * cy}};
    Mat m;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            m.m[i][j] = R[i][j] * s[j];
        }
        m.m[i][3] = t[i];
    }
    return m;
}

static const int kMaxJoints = 256;
static const int kMaxDraw = 1024;

// Walks INF1's scene graph and computes each joint's world matrix. Returns the number of joints (0 = no skeleton data).
static uint32_t computeJoints(const uint8_t* inf, uint32_t infSize, const uint8_t* jnt, uint32_t jntSize, Mat* world) {
    if (!inf || !jnt || infSize < 0x18 || jntSize < 0x18) {
        return 0;
    }
    uint32_t count = sdfs_be16(jnt + 8);
    if (count == 0 || count > (uint32_t)kMaxJoints) {
        return 0;
    }
    uint32_t initOff = sdfs_be32(jnt + 0x0C), idxTab = sdfs_be32(jnt + 0x10);
    for (uint32_t j = 0; j < count; j++) {
        world[j] = matIdentity();
    }
    int stack[64];
    int sp = 0;
    int cur = -1;
    uint32_t hier = sdfs_be32(inf + 0x14);
    for (uint32_t p = hier; fits(infSize, p, 4); p += 4) {
        uint32_t type = sdfs_be16(inf + p), val = sdfs_be16(inf + p + 2);
        if (type == 0) break;
        if (type == 0x10 && val < count) {
            int parent = sp ? stack[sp - 1] : -1;
            uint32_t ii = val;
            if (fits(jntSize, idxTab + 2ull * val, 2)) ii = sdfs_be16(jnt + idxTab + 2 * val);
            uint64_t ip = (uint64_t)initOff + (uint64_t)ii * 0x40;
            Mat local = matIdentity();
            if (fits(jntSize, ip, 0x40)) {
                const uint8_t* q = jnt + ip;
                float s[3] = {readF(q + 4), readF(q + 8), readF(q + 12)};
                const float k = 3.14159265f / 32768.0f;
                float r[3] = {(int16_t)sdfs_be16(q + 0x10) * k, (int16_t)sdfs_be16(q + 0x12) * k, (int16_t)sdfs_be16(q + 0x14) * k};
                float t[3] = {readF(q + 0x18), readF(q + 0x1C), readF(q + 0x20)};
                local = matFromSRT(s, r, t);
            }
            world[val] = (parent >= 0 && (uint32_t)parent < count) ? matMul(world[parent], local) : local;
            cur = (int)val;
        } else if (type == 1) {
            if (sp < 64) stack[sp++] = cur;
        } else if (type == 2) {
            if (sp) sp--;
        }
    }
    return count;
}

// Matrix for every DRW1 entry: a joint matrix, or a weighted blend of joints (EVP1) for skinned vertices.
static uint32_t computeDrawMatrices(const uint8_t* drw, uint32_t drwSize, const uint8_t* evp, uint32_t evpSize,
                                    const Mat* world, uint32_t jointCount, Mat* out) {
    if (!drw || drwSize < 0x14) {
        return 0;
    }
    uint32_t n = sdfs_be16(drw + 8);
    if (n > (uint32_t)kMaxDraw) {
        n = kMaxDraw;
    }
    uint32_t flagOff = sdfs_be32(drw + 0x0C), idxOff = sdfs_be32(drw + 0x10);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = matIdentity();
        if (!fits(drwSize, flagOff + i, 1) || !fits(drwSize, idxOff + 2ull * i, 2)) {
            continue;
        }
        uint32_t flag = drw[flagOff + i];
        uint32_t idx = sdfs_be16(drw + idxOff + 2 * i);
        if (flag == 0) {
            if (idx < jointCount) out[i] = world[idx];
        } else if (evp && evpSize >= 0x1C && idx < sdfs_be16(evp + 8)) {
            uint32_t numOff = sdfs_be32(evp + 0x0C), idxTab = sdfs_be32(evp + 0x10), wOff = sdfs_be32(evp + 0x14),
                     invOff = sdfs_be32(evp + 0x18);
            uint32_t first = 0;
            for (uint32_t e = 0; e < idx; e++) {
                if (!fits(evpSize, numOff + e, 1)) break;
                first += evp[numOff + e];
            }
            if (!fits(evpSize, numOff + idx, 1)) continue;
            uint32_t cnt = evp[numOff + idx];
            Mat acc;
            memset(&acc, 0, sizeof(acc));
            bool any = false;
            for (uint32_t k = 0; k < cnt; k++) {
                if (!fits(evpSize, idxTab + 2ull * (first + k), 2) || !fits(evpSize, wOff + 4ull * (first + k), 4)) break;
                uint32_t j = sdfs_be16(evp + idxTab + 2 * (first + k));
                float w = readF(evp + wOff + 4 * (first + k));
                if (j >= jointCount || !fits(evpSize, invOff + 48ull * (j + 1), 0)) continue;
                if (!fits(evpSize, invOff + 48ull * j, 48)) continue;
                Mat inv = matIdentity();
                for (int r = 0; r < 3; r++)
                    for (int c2 = 0; c2 < 4; c2++) inv.m[r][c2] = readF(evp + invOff + 48 * j + 16 * r + 4 * c2);
                Mat blend = matMul(world[j], inv);
                for (int r = 0; r < 3; r++)
                    for (int c2 = 0; c2 < 4; c2++) acc.m[r][c2] += w * blend.m[r][c2];
                any = true;
            }
            if (any) out[i] = acc;
        }
    }
    return n;
}

struct ShapeCtx {
    ArrFmt pos, uv;
    bool hasUv;
    int16_t tex;
    const Mat* draw;      // DRW1 matrices (may be null)
    uint32_t drawCount;
};

static void emitTri(MeshBuilder& b, const ShapeCtx& s, const uint32_t* pi, const uint32_t* ti, const uint32_t* pm,
                    uint32_t i0, uint32_t i1, uint32_t i2) {
    uint32_t ids[3] = {i0, i1, i2};
    float p[9], uv[6] = {0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 3; k++) {
        if (!readElem(s.pos, pi[ids[k]], p + k * 3)) {
            return;
        }
        uint32_t d = pm[ids[k]];
        if (s.draw && d < s.drawCount) {
            const Mat& M = s.draw[d];
            float x = p[k * 3], y = p[k * 3 + 1], z = p[k * 3 + 2];
            p[k * 3] = M.m[0][0] * x + M.m[0][1] * y + M.m[0][2] * z + M.m[0][3];
            p[k * 3 + 1] = M.m[1][0] * x + M.m[1][1] * y + M.m[1][2] * z + M.m[1][3];
            p[k * 3 + 2] = M.m[2][0] * x + M.m[2][1] * y + M.m[2][2] * z + M.m[2][3];
        }
        if (s.hasUv) {
            float t[3];
            if (readElem(s.uv, ti[ids[k]], t)) {
                uv[k * 2] = t[0];
                uv[k * 2 + 1] = t[1];
            }
        }
    }
    addTri(b, p, uv, s.hasUv ? s.tex : (int16_t)-1);
}

// ---- texture decoding --------------------------------------------------------------------------

static inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (r << 24) | (g << 16) | (b << 8) | a;
}
static uint32_t col565(uint16_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return pack((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}
static uint32_t col5a3(uint16_t v) {
    if (v & 0x8000) {
        uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return pack((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
    }
    uint32_t a = (v >> 12) & 7, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
    return pack(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
}
static uint32_t colIA8(uint16_t v) {
    uint32_t i = v & 255;
    return pack(i, i, i, v >> 8);
}
static uint32_t palColor(uint32_t fmt, uint16_t v) {
    return fmt == 0 ? colIA8(v) : (fmt == 1 ? col565(v) : col5a3(v));
}

// hdr = 0x20-byte BTI header; avail = bytes available from hdr to the end of the chunk.
static bool decodeBti(const uint8_t* hdr, size_t avail, J3dTexture* out) {
    if (avail < 0x20) return false;
    uint32_t fmt = hdr[0];
    uint32_t w = sdfs_be16(hdr + 2), h = sdfs_be16(hdr + 4);
    if (!w || !h || w > 1024 || h > 1024) return false;
    uint32_t bw, bh, bytes;
    switch (fmt) {
        case 0: bw = 8; bh = 8; bytes = 32; break;   // I4
        case 1: bw = 8; bh = 4; bytes = 32; break;   // I8
        case 2: bw = 8; bh = 4; bytes = 32; break;   // IA4
        case 3: bw = 4; bh = 4; bytes = 32; break;   // IA8
        case 4: bw = 4; bh = 4; bytes = 32; break;   // RGB565
        case 5: bw = 4; bh = 4; bytes = 32; break;   // RGB5A3
        case 6: bw = 4; bh = 4; bytes = 64; break;   // RGBA32
        case 8: bw = 8; bh = 8; bytes = 32; break;   // C4
        case 9: bw = 8; bh = 4; bytes = 32; break;   // C8
        case 10: bw = 4; bh = 4; bytes = 32; break;  // C14X2
        case 14: bw = 8; bh = 8; bytes = 32; break;  // CMPR
        default: return false;
    }
    uint32_t imgOff = sdfs_be32(hdr + 0x1C);
    uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    if (!fits(avail, imgOff, (uint64_t)bx * by * bytes)) return false;
    const uint8_t* data = hdr + imgOff;

    const uint8_t* pal = nullptr;
    uint32_t palN = 0, palFmt = hdr[9];
    if (fmt == 8 || fmt == 9 || fmt == 10) {
        palN = sdfs_be16(hdr + 0x0A);
        uint32_t po = sdfs_be32(hdr + 0x0C);
        if (!palN || !fits(avail, po, (uint64_t)palN * 2)) return false;
        pal = hdr + po;
    }

    uint32_t* pix = (uint32_t*)malloc((size_t)w * h * 4);
    if (!pix) return false;
    memset(pix, 0, (size_t)w * h * 4);

    for (uint32_t ty = 0; ty < by; ty++) {
        for (uint32_t tx = 0; tx < bx; tx++) {
            const uint8_t* blk = data + ((size_t)ty * bx + tx) * bytes;
            for (uint32_t py = 0; py < bh; py++) {
                uint32_t y = ty * bh + py;
                if (y >= h) break;
                for (uint32_t px = 0; px < bw; px++) {
                    uint32_t x = tx * bw + px;
                    if (x >= w) break;
                    uint32_t c = 0;
                    uint32_t i = py * bw + px;
                    switch (fmt) {
                        case 0: {
                            uint32_t n = (blk[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                            c = pack(n * 17, n * 17, n * 17, 255);
                            break;
                        }
                        case 1: c = pack(blk[i], blk[i], blk[i], 255); break;
                        case 2: {
                            uint32_t a = blk[i] >> 4, n = blk[i] & 15;
                            c = pack(n * 17, n * 17, n * 17, a * 17);
                            break;
                        }
                        case 3: c = colIA8(sdfs_be16(blk + 2 * i)); break;
                        case 4: c = col565(sdfs_be16(blk + 2 * i)); break;
                        case 5: c = col5a3(sdfs_be16(blk + 2 * i)); break;
                        case 6: c = pack(blk[2 * i + 1], blk[32 + 2 * i], blk[32 + 2 * i + 1], blk[2 * i]); break;
                        case 8: {
                            uint32_t n = (blk[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                            c = n < palN ? palColor(palFmt, sdfs_be16(pal + 2 * n)) : 0;
                            break;
                        }
                        case 9: {
                            uint32_t n = blk[i];
                            c = n < palN ? palColor(palFmt, sdfs_be16(pal + 2 * n)) : 0;
                            break;
                        }
                        case 10: {
                            uint32_t n = sdfs_be16(blk + 2 * i) & 0x3FFF;
                            c = n < palN ? palColor(palFmt, sdfs_be16(pal + 2 * n)) : 0;
                            break;
                        }
                        case 14: {
                            // four 4x4 sub-blocks (row-major) of 8 bytes: two RGB565 colours + 2-bit indices
                            const uint8_t* sb = blk + (((py >> 2) << 1) + (px >> 2)) * 8;
                            uint16_t c0 = sdfs_be16(sb), c1 = sdfs_be16(sb + 2);
                            uint32_t k0 = col565(c0), k1 = col565(c1);
                            uint32_t sel = (sb[4 + (py & 3)] >> (6 - 2 * (px & 3))) & 3;
                            if (sel == 0) c = k0;
                            else if (sel == 1) c = k1;
                            else {
                                uint32_t ch[3];
                                for (int s = 0; s < 3; s++) {
                                    uint32_t sh = 24 - 8 * s;
                                    uint32_t a = (k0 >> sh) & 255, b = (k1 >> sh) & 255;
                                    if (c0 > c1) ch[s] = sel == 2 ? (2 * a + b) / 3 : (a + 2 * b) / 3;
                                    else ch[s] = (a + b) / 2;
                                }
                                c = (c0 <= c1 && sel == 3) ? 0 : pack(ch[0], ch[1], ch[2], 255);
                            }
                            break;
                        }
                    }
                    pix[(size_t)y * w + x] = c;
                }
            }
        }
    }
    out->w = (uint16_t)w;
    out->h = (uint16_t)h;
    out->wrapS = hdr[6];
    out->wrapT = hdr[7];
    out->rgba = pix;
    return true;
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
    const uint8_t *inf = nullptr, *vtx = nullptr, *shp = nullptr, *mat = nullptr, *tex = nullptr;
    const uint8_t *jnt = nullptr, *drw = nullptr, *evp = nullptr;
    uint32_t infSize = 0, vtxSize = 0, shpSize = 0, matSize = 0, texSize = 0, jntSize = 0, drwSize = 0, evpSize = 0;
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
        if (memcmp(c, "INF1", 4) == 0) { inf = c; infSize = csize; }
        else if (memcmp(c, "VTX1", 4) == 0) { vtx = c; vtxSize = csize; }
        else if (memcmp(c, "SHP1", 4) == 0) { shp = c; shpSize = csize; }
        else if (memcmp(c, "MAT3", 4) == 0) { mat = c; matSize = csize; }
        else if (memcmp(c, "TEX1", 4) == 0) { tex = c; texSize = csize; }
        else if (memcmp(c, "JNT1", 4) == 0) { jnt = c; jntSize = csize; }
        else if (memcmp(c, "DRW1", 4) == 0) { drw = c; drwSize = csize; }
        else if (memcmp(c, "EVP1", 4) == 0) { evp = c; evpSize = csize; }
        off += csize;
    }
    if (!vtx || !shp || vtxSize < 0x4C || shpSize < 0x2C) {
        return false;
    }

    ShapeCtx sc;
    sc.pos = readArray(vtx, vtxSize, 9, 0, 2, 3);
    sc.uv = readArray(vtx, vtxSize, 13, 5, 1, 2);  // TEX0 array is header slot 5 (0x0C + 4*5 = 0x20)
    sc.hasUv = sc.uv.valid;
    sc.tex = -1;
    sc.draw = nullptr;
    sc.drawCount = 0;
    if (!sc.pos.valid) {
        return false;
    }

    // ---- which texture does each shape use? INF1 scene graph -> material -> MAT3 -> TEX1 index
    uint32_t shapeCount = sdfs_be16(shp + 8);
    uint32_t shapeOff = sdfs_be32(shp + 0x0C);
    uint32_t shapeIdxTab = sdfs_be32(shp + 0x10);
    int32_t* shapeTexIdx = (int32_t*)malloc(sizeof(int32_t) * (shapeCount + 1));  // TEX1 index per shape init entry
    uint32_t* idx = (uint32_t*)malloc(3 * 65536 * sizeof(uint32_t));
    Mat* joints = (Mat*)malloc(sizeof(Mat) * (kMaxJoints + kMaxDraw));
    int16_t slotOf[256];
    if (!shapeTexIdx || !idx || !joints) {
        free(shapeTexIdx);
        free(idx);
        free(joints);
        return false;
    }
    Mat* drawMtx = joints + kMaxJoints;
    uint32_t jointCount = computeJoints(inf, infSize, jnt, jntSize, joints);
    uint32_t drawCount = jointCount ? computeDrawMatrices(drw, drwSize, evp, evpSize, joints, jointCount, drawMtx) : 0;
    sc.draw = drawCount ? drawMtx : nullptr;
    sc.drawCount = drawCount;
    for (uint32_t s = 0; s <= shapeCount; s++) shapeTexIdx[s] = -1;
    for (int k = 0; k < 256; k++) slotOf[k] = -1;

    if (inf && mat && tex && infSize >= 0x18 && matSize >= 0x4C) {
        uint32_t matCount = sdfs_be16(mat + 8);
        uint32_t hier = sdfs_be32(inf + 0x14);
        int32_t curMat = -1;
        for (uint32_t p = hier; fits(infSize, p, 4); p += 4) {
            uint32_t type = sdfs_be16(inf + p), val = sdfs_be16(inf + p + 2);
            if (type == 0) break;
            if (type == 0x11) {
                curMat = (int32_t)val;
            } else if (type == 0x12 && curMat >= 0 && (uint32_t)curMat < matCount) {
                uint32_t initIdx = 0;
                uint32_t idTab = sdfs_be32(mat + 0x10);
                if (!fits(matSize, idTab + 2ull * curMat, 2)) continue;
                initIdx = sdfs_be16(mat + idTab + 2 * curMat);
                uint64_t ip = (uint64_t)sdfs_be32(mat + 0x0C) + (uint64_t)initIdx * 0x14C;
                if (!fits(matSize, ip, 0x14C)) continue;
                uint32_t ti = sdfs_be16(mat + ip + 0x84);
                if (ti == 0xFFFF) continue;
                uint32_t tn = sdfs_be32(mat + 0x48);
                if (!fits(matSize, tn + 2ull * ti, 2)) continue;
                uint32_t texNo = sdfs_be16(mat + tn + 2 * ti);
                // shape node value -> shape init entry through SHP1's index table
                uint32_t initShape = val;
                if (shapeIdxTab && fits(shpSize, shapeIdxTab + 2ull * val, 2)) {
                    initShape = sdfs_be16(shp + shapeIdxTab + 2 * val);
                }
                if (initShape < shapeCount) {
                    shapeTexIdx[initShape] = (int32_t)texNo;
                }
            }
        }
    }

    MeshBuilder mb;
    mb.mesh = out;

    uint32_t* pidx = idx;
    uint32_t* tidx = idx + 65536;
    uint32_t* midx = idx + 2 * 65536;
    uint32_t mtxTableOff = sdfs_be32(shp + 0x1C);
    uint32_t mtxInitBase = sdfs_be32(shp + 0x24);
    uint32_t attrBase = sdfs_be32(shp + 0x18);
    uint32_t drawBase = sdfs_be32(shp + 0x20);
    uint32_t packBase = sdfs_be32(shp + 0x28);

    uint32_t texCount1 = tex ? sdfs_be16(tex + 8) : 0;
    uint32_t texHdrOff = tex ? sdfs_be32(tex + 0x0C) : 0;

    for (uint32_t s = 0; s < shapeCount; s++) {
        uint64_t sp = (uint64_t)shapeOff + (uint64_t)s * 0x28;
        if (!fits(shpSize, sp, 0x28)) {
            break;
        }
        uint32_t groups = sdfs_be16(shp + sp + 2);
        uint32_t attrOff = sdfs_be16(shp + sp + 4);
        uint32_t firstGroup = sdfs_be16(shp + sp + 8);

        uint32_t vertBytes = 0, posByte = 0, posKind = 0, uvByte = 0, uvKind = 0, pnByte = 0;
        bool hasPn = false;
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
            if (attr == 0 && sz == 1) {
                pnByte = vertBytes;
                hasPn = true;
            }
            if (attr == 9 && sz) {
                posByte = vertBytes;
                posKind = kind;
            }
            if (attr == 13 && sz) {
                uvByte = vertBytes;
                uvKind = kind;
            }
            vertBytes += sz;
        }
        if (!ok || vertBytes == 0 || posKind < 2) {
            continue;
        }

        // resolve this shape's texture into a slot in out->textures (decoded on first use)
        ShapeCtx cur = sc;
        cur.hasUv = sc.hasUv && uvKind >= 2;
        cur.tex = -1;
        int32_t ti = shapeTexIdx[s];
        if (cur.hasUv && ti >= 0 && (uint32_t)ti < texCount1 && ti < 256) {
            if (slotOf[ti] < 0) {
                uint64_t ho = (uint64_t)texHdrOff + (uint64_t)ti * 0x20;
                J3dTexture t;
                memset(&t, 0, sizeof(t));
                if (fits(texSize, ho, 0x20) && out->texCount < 32 && decodeBti(tex + ho, texSize - ho, &t)) {
                    J3dTexture* grown = (J3dTexture*)realloc(out->textures, (out->texCount + 1) * sizeof(J3dTexture));
                    if (grown) {
                        out->textures = grown;
                        out->textures[out->texCount] = t;
                        slotOf[ti] = (int16_t)out->texCount++;
                    } else {
                        free(t.rgba);
                    }
                }
            }
            cur.tex = slotOf[ti];
        }

        uint32_t firstMtxInit = sdfs_be16(shp + sp + 6);
        for (uint32_t g = 0; g < groups; g++) {
            // matrix group: which DRW1 entries this packet's PNMTXIDX values refer to
            uint32_t useCount = 0, firstUse = 0;
            uint64_t mi = (uint64_t)mtxInitBase + (uint64_t)(firstMtxInit + g) * 8;
            if (mtxInitBase && fits(shpSize, mi, 8)) {
                useCount = sdfs_be16(shp + mi + 2);
                firstUse = sdfs_be32(shp + mi + 4);
            }
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
                    const uint8_t* vp = shp + dl + pos + (size_t)v * vertBytes;
                    pidx[v] = (posKind == 3) ? sdfs_be16(vp + posByte) : vp[posByte];
                    if (cur.hasUv) {
                        tidx[v] = (uvKind == 3) ? sdfs_be16(vp + uvByte) : vp[uvByte];
                    }
                    uint32_t slot = hasPn ? vp[pnByte] / 3 : 0;
                    uint32_t drwIdx = 0xFFFF;
                    if (slot < useCount && fits(shpSize, mtxTableOff + 2ull * (firstUse + slot), 2)) {
                        drwIdx = sdfs_be16(shp + mtxTableOff + 2 * (firstUse + slot));
                    }
                    midx[v] = drwIdx;
                }
                pos += n * vertBytes;

                switch (cmd & 0xF8) {
                    case 0x90:  // triangles
                        for (uint32_t i = 0; i + 2 < n; i += 3) {
                            emitTri(mb, cur, pidx, tidx, midx, i, i + 1, i + 2);
                        }
                        break;
                    case 0x98:  // triangle strip
                        for (uint32_t i = 0; i + 2 < n; i++) {
                            if (i & 1) {
                                emitTri(mb, cur, pidx, tidx, midx, i + 1, i, i + 2);
                            } else {
                                emitTri(mb, cur, pidx, tidx, midx, i, i + 1, i + 2);
                            }
                        }
                        break;
                    case 0xA0:  // triangle fan
                        for (uint32_t i = 1; i + 1 < n; i++) {
                            emitTri(mb, cur, pidx, tidx, midx, 0, i, i + 1);
                        }
                        break;
                    case 0x80:  // quads
                        for (uint32_t i = 0; i + 3 < n; i += 4) {
                            emitTri(mb, cur, pidx, tidx, midx, i, i + 1, i + 2);
                            emitTri(mb, cur, pidx, tidx, midx, i, i + 2, i + 3);
                        }
                        break;
                    default: break;
                }
            }
        }
    }
    free(idx);
    free(joints);
    free(shapeTexIdx);
    if (out->triCount == 0) {
        j3d_free_mesh(out);
        return false;
    }
    return true;
}

void j3d_free_mesh(J3dMesh* m) {
    for (uint32_t i = 0; i < m->texCount; i++) {
        free(m->textures[i].rgba);
    }
    free(m->textures);
    free(m->tris);
    free(m->uvs);
    free(m->triTex);
    memset(m, 0, sizeof(*m));
}
