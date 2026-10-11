// Minimal J3D model inspector (.bmd / .bdl). Reads headers only: it counts what is inside a model. All fields are big-endian.
// This is the first step toward drawing models; nothing here allocates or decodes vertex data yet.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct J3dInfo {
    char type[5];            // "bmd3" or "bdl4"
    uint32_t chunkCount;
    uint32_t chunksParsed;   // chunks that fit inside the file
    uint32_t infVertexCount; // INF1: vertex count of the scene graph
    uint32_t posCount;       // VTX1: approx. position count (the array is padded, so this can be slightly high)
    uint32_t posComps;       // 2 (XY) or 3 (XYZ)
    uint32_t posType;        // 0=u8 1=s8 2=u16 3=s16 4=f32
    uint32_t shapeCount;     // SHP1
    uint32_t triangles;      // triangles decoded from the display lists
    uint32_t packets;        // display-list packets
    uint32_t materialCount;  // MAT3
    uint32_t textureCount;   // TEX1
    bool unsupportedAttr;    // a shape uses a vertex attribute layout this reader doesn't understand
};

// Returns false if data is not a J3D file. Safe on truncated or corrupt input (every offset is bounds-checked).
bool j3d_inspect(const uint8_t* data, size_t size, J3dInfo* out);

// A decoded texture, RGBA8 packed as 0xRRGGBBAA, row-major, first row = top of the image.
struct J3dTexture {
    uint16_t w, h;
    uint8_t wrapS, wrapT;  // 0 clamp, 1 repeat, 2 mirror
    uint32_t* rgba;
};

// A decoded model: a plain triangle list. Static geometry only for now: joint/skinning matrices are not applied,
// so multi-joint models can look pulled apart.
struct J3dMesh {
    float* tris;        // triCount * 9 floats (3 vertices x xyz)
    float* uvs;         // triCount * 6 floats (3 vertices x st), GX convention (t = 0 at the top of the image)
    int16_t* triTex;    // triCount entries: index into textures[], or -1 for none
    uint32_t triCount;
    float minv[3];
    float maxv[3];
    J3dTexture* textures;  // only textures actually used by some shape
    uint32_t texCount;
};

// Decodes positions, texture coordinates, display lists and the textures they use. False if nothing is drawable.
bool j3d_build_mesh(const uint8_t* data, size_t size, J3dMesh* out);
void j3d_free_mesh(J3dMesh* m);
