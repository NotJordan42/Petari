// citro3d model viewer: spins one decoded model on the top screen. Plain 3DS code only (see the Makefile note on PLAIN_CXXFLAGS).
#include "gpuview.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>

#include "gpumodel.h"

static GpuCtx g_ctx;
static GpuModel g_model;

const char* gpuview_run(const J3dMesh& mesh) {
    const char* err = gpuctx_init(&g_ctx);
    if (err) {
        return err;
    }
    float cx = 0.5f * (mesh.minv[0] + mesh.maxv[0]);
    float cy = 0.5f * (mesh.minv[1] + mesh.maxv[1]);
    float cz = 0.5f * (mesh.minv[2] + mesh.maxv[2]);
    float ex = mesh.maxv[0] - mesh.minv[0], ey = mesh.maxv[1] - mesh.minv[1], ez = mesh.maxv[2] - mesh.minv[2];
    float radius = 0.5f * sqrtf(ex * ex + ey * ey + ez * ez);
    float center[3] = {cx, cy, cz};
    err = gpumodel_create(&g_model, mesh, center, radius > 1e-6f ? 1.0f / radius : 1.0f);
    if (err) {
        gpuctx_free(&g_ctx);
        return err;
    }

    static const float kBlue[4] = {0.59f, 0.75f, 1.0f, 1.0f};
    float yaw = 0.0f, pitch = 0.3f, zoom = 1.0f;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        if (down & KEY_B) {
            break;
        }
        if (held & KEY_DLEFT) yaw -= 0.05f; else if (held & KEY_DRIGHT) yaw += 0.05f; else yaw += 0.015f;
        if (held & KEY_DUP) pitch += 0.04f;
        if (held & KEY_DDOWN) pitch -= 0.04f;
        if (held & KEY_R) zoom *= 1.03f;
        if (held & KEY_L) zoom /= 1.03f;

        C3D_Mtx mv;
        Mtx_Identity(&mv);
        Mtx_Translate(&mv, 0.0f, 0.0f, -3.0f / zoom, true);
        Mtx_RotateX(&mv, pitch, true);
        Mtx_RotateY(&mv, yaw, true);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(g_ctx.tgt, C3D_CLEAR_ALL, 0x101828FF, 0);
        C3D_FrameDrawOn(g_ctx.tgt);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_ctx.locProj, &g_ctx.proj);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_ctx.locMv, &mv);
        gpumodel_draw(g_model, g_ctx, kBlue);
        C3D_FrameEnd(0);
    }

    gpumodel_free(&g_model);
    gpuctx_free(&g_ctx);
    return nullptr;
}
