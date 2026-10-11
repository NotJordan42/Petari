// Shared citro3d plumbing: render context + uploading a decoded J3dMesh to the GPU. Plain 3DS code (no Petari headers).
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include "j3d.h"

struct GpuCtx {
    C3D_RenderTarget* tgt;
    DVLB_s* dvlb;
    shaderProgram_s prog;
    int locProj, locMv, locTint;
    C3D_Mtx proj;
};

// Starts citro3d on the top screen. Returns nullptr on success or a short error message.
const char* gpuctx_init(GpuCtx* c);
void gpuctx_free(GpuCtx* c);

struct GpuModel {
    void* vbo;
    uint32_t groups;
    uint32_t groupStart[34];
    uint32_t groupCount[34];
    C3D_Tex tex[32];
    bool texOk[32];
    uint32_t texCount;
};

// Vertices are stored as (p - center) * scale. Returns nullptr on success or an error message.
const char* gpumodel_create(GpuModel* m, const J3dMesh& mesh, const float center[3], float scale);
void gpumodel_free(GpuModel* m);

// Draws with the modelview already set on the context. untexTint multiplies untextured parts (rgba, 0..1).
void gpumodel_draw(const GpuModel& m, const GpuCtx& c, const float untexTint[4]);
