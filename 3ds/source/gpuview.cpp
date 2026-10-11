// citro3d model viewer. Plain 3DS code only (no Petari headers), see the Makefile note on PLAIN_CXXFLAGS.
#include "gpuview.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>

extern const u8 vshader_shbin[];
extern const u32 vshader_shbin_size;

#define DISPLAY_TRANSFER_FLAGS                                                                                          \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                    \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                      \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

struct Vtx {
    float x, y, z;
    float nx, ny, nz;
};

const char* gpuview_run(const J3dMesh& mesh) {
    if (!mesh.triCount) {
        return "no mesh";
    }
    size_t vcount = (size_t)mesh.triCount * 3;
    Vtx* vbo = (Vtx*)linearAlloc(vcount * sizeof(Vtx));
    if (!vbo) {
        return "out of linear memory";
    }

    // Centre the model on the origin and scale it to a radius of 1, with a flat normal per triangle.
    float cx = 0.5f * (mesh.minv[0] + mesh.maxv[0]);
    float cy = 0.5f * (mesh.minv[1] + mesh.maxv[1]);
    float cz = 0.5f * (mesh.minv[2] + mesh.maxv[2]);
    float ex = mesh.maxv[0] - mesh.minv[0], ey = mesh.maxv[1] - mesh.minv[1], ez = mesh.maxv[2] - mesh.minv[2];
    float radius = 0.5f * sqrtf(ex * ex + ey * ey + ez * ez);
    float inv = radius > 1e-6f ? 1.0f / radius : 1.0f;
    for (uint32_t t = 0; t < mesh.triCount; t++) {
        const float* v = mesh.tris + (size_t)t * 9;
        Vtx* o = vbo + (size_t)t * 3;
        for (int i = 0; i < 3; i++) {
            o[i].x = (v[i * 3] - cx) * inv;
            o[i].y = (v[i * 3 + 1] - cy) * inv;
            o[i].z = (v[i * 3 + 2] - cz) * inv;
        }
        float ax = o[1].x - o[0].x, ay = o[1].y - o[0].y, az = o[1].z - o[0].z;
        float bx = o[2].x - o[0].x, by = o[2].y - o[0].y, bz = o[2].z - o[0].z;
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-12f) {
            nx = 0; ny = 0; nz = 1;
        } else {
            nx /= nl; ny /= nl; nz /= nl;
        }
        for (int i = 0; i < 3; i++) {
            o[i].nx = nx;
            o[i].ny = ny;
            o[i].nz = nz;
        }
    }
    GSPGPU_FlushDataCache(vbo, vcount * sizeof(Vtx));

    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSet3D(false);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        linearFree(vbo);
        return "C3D_Init failed";
    }
    C3D_RenderTarget* tgt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!tgt) {
        C3D_Fini();
        linearFree(vbo);
        return "render target failed";
    }
    C3D_RenderTargetSetOutput(tgt, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    DVLB_s* dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    if (!dvlb) {
        C3D_RenderTargetDelete(tgt);
        C3D_Fini();
        linearFree(vbo);
        return "shader parse failed";
    }
    shaderProgram_s prog;
    shaderProgramInit(&prog);
    shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
    C3D_BindProgram(&prog);

    int locProj = shaderInstanceGetUniformLocation(prog.vertexShader, "projection");
    int locMv = shaderInstanceGetUniformLocation(prog.vertexShader, "modelview");

    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);

    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, vbo, sizeof(Vtx), 2, 0x10);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_CullFace(GPU_CULL_NONE);

    C3D_Mtx proj;
    Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(40.0f), C3D_AspectRatioTop, 0.1f, 100.0f, false);

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
        C3D_RenderTargetClear(tgt, C3D_CLEAR_ALL, 0x101828FF, 0);
        C3D_FrameDrawOn(tgt);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, locProj, &proj);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, locMv, &mv);
        C3D_DrawArrays(GPU_TRIANGLES, 0, (int)vcount);
        C3D_FrameEnd(0);
    }

    shaderProgramFree(&prog);
    DVLB_Free(dvlb);
    C3D_RenderTargetDelete(tgt);
    C3D_Fini();
    linearFree(vbo);
    return nullptr;
}
