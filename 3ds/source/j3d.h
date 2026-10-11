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
