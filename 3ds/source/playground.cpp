// Mario walk-around: circle pad moves, A jumps, D-pad left/right turns the camera, L/R zooms, B exits.
#include "playground.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gpumodel.h"

static GpuCtx g_ctx;
static GpuModel g_player, g_groundA, g_groundB;

static const int kSquares = 16;      // per side
static const float kSquareSize = 3;  // metres
static const float kHalf = kSquares * kSquareSize * 0.5f;

// Builds a flat checkerboard in world units: only the squares of one parity (0 or 1), two triangles each.
static bool makeGround(J3dMesh* m, int parity) {
    memset(m, 0, sizeof(*m));
    int maxTris = kSquares * kSquares;  // 2 triangles x (N*N/2 squares)
    m->tris = (float*)malloc(sizeof(float) * 9 * maxTris);
    m->uvs = (float*)calloc(maxTris, sizeof(float) * 6);
    m->triTex = (int16_t*)malloc(sizeof(int16_t) * maxTris);
    if (!m->tris || !m->uvs || !m->triTex) {
        free(m->tris); free(m->uvs); free(m->triTex);
        memset(m, 0, sizeof(*m));
        return false;
    }
    uint32_t n = 0;
    for (int i = 0; i < kSquares; i++) {
        for (int j = 0; j < kSquares; j++) {
            if (((i + j) & 1) != parity) continue;
            float x0 = -kHalf + i * kSquareSize, x1 = x0 + kSquareSize;
            float z0 = -kHalf + j * kSquareSize, z1 = z0 + kSquareSize;
            const float a[9] = {x0, 0, z0, x1, 0, z0, x0, 0, z1};
            const float b[9] = {x1, 0, z0, x1, 0, z1, x0, 0, z1};
            memcpy(m->tris + (size_t)n * 9, a, sizeof(a));
            m->triTex[n++] = -1;
            memcpy(m->tris + (size_t)n * 9, b, sizeof(b));
            m->triTex[n++] = -1;
        }
    }
    m->triCount = n;
    return n > 0;
}

static float wrapAngle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

const char* playground_run(const J3dMesh& player) {
    if (!player.triCount) {
        return "no player mesh";
    }
    J3dMesh gA, gB;
    if (!makeGround(&gA, 0) || !makeGround(&gB, 1)) {
        return "out of memory (ground)";
    }
    const char* err = gpuctx_init(&g_ctx);
    if (err) {
        j3d_free_mesh(&gA);
        j3d_free_mesh(&gB);
        return err;
    }

    // Player: feet on y = 0, centred on x/z, scaled to ~1.7 m tall. (If the model's height is tiny or huge this fixes it.)
    float height = player.maxv[1] - player.minv[1];
    float pscale = height > 1e-6f ? 1.7f / height : 1.0f;
    float pcenter[3] = {0.5f * (player.minv[0] + player.maxv[0]), player.minv[1], 0.5f * (player.minv[2] + player.maxv[2])};
    const float zero[3] = {0, 0, 0};
    err = gpumodel_create(&g_player, player, pcenter, pscale);
    if (!err) err = gpumodel_create(&g_groundA, gA, zero, 1.0f);
    if (!err) err = gpumodel_create(&g_groundB, gB, zero, 1.0f);
    j3d_free_mesh(&gA);
    j3d_free_mesh(&gB);
    if (err) {
        gpumodel_free(&g_player);
        gpumodel_free(&g_groundA);
        gpumodel_free(&g_groundB);
        gpuctx_free(&g_ctx);
        return err;
    }

    static const float kGrassA[4] = {0.40f, 0.78f, 0.38f, 1.0f};
    static const float kGrassB[4] = {0.30f, 0.64f, 0.32f, 1.0f};
    static const float kPlayerTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    float px = 0, py = 0, pz = 0, vy = 0, face = 0;
    float camYaw = 0, zoom = 1.0f;
    const float dt = 1.0f / 60.0f;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        if (down & KEY_B) {
            break;
        }
        if (held & KEY_DLEFT) camYaw -= 2.0f * dt;
        if (held & KEY_DRIGHT) camYaw += 2.0f * dt;
        if (held & KEY_R) zoom *= 1.02f;
        if (held & KEY_L) zoom /= 1.02f;
        if (zoom < 0.4f) zoom = 0.4f;
        if (zoom > 3.0f) zoom = 3.0f;

        circlePosition cp;
        hidCircleRead(&cp);
        float sx = cp.dx / 156.0f, sy = cp.dy / 156.0f;
        float mag = sqrtf(sx * sx + sy * sy);
        if (mag < 0.12f) {
            sx = sy = mag = 0;
        } else if (mag > 1.0f) {
            sx /= mag; sy /= mag; mag = 1.0f;
        }
        if (mag > 0) {
            // stick is relative to the camera: up = away from the camera
            float mx = cosf(camYaw) * sx - sinf(camYaw) * sy;
            float mz = -sinf(camYaw) * sx - cosf(camYaw) * sy;
            const float speed = 7.0f;
            px += mx * speed * dt;
            pz += mz * speed * dt;
            float target = atan2f(mx, mz);
            float d = wrapAngle(target - face);
            float step = 14.0f * dt;
            face = fabsf(d) < step ? target : face + (d > 0 ? step : -step);
            face = wrapAngle(face);
        }
        if (px > kHalf) px = kHalf;
        if (px < -kHalf) px = -kHalf;
        if (pz > kHalf) pz = kHalf;
        if (pz < -kHalf) pz = -kHalf;

        bool onGround = py <= 0.0f;
        if (onGround && (down & KEY_A)) {
            vy = 9.0f;
            onGround = false;
        }
        if (!onGround || vy > 0) {
            vy -= 26.0f * dt;
            py += vy * dt;
            if (py <= 0.0f) {
                py = 0.0f;
                vy = 0.0f;
            }
        }

        // camera: orbits the player. V = T(0,0,-D) * Rx(pitch) * Ry(-yaw) * T(-target)
        float dist = 7.0f * zoom;
        C3D_Mtx view;
        Mtx_Identity(&view);
        Mtx_Translate(&view, 0.0f, 0.0f, -dist, true);
        Mtx_RotateX(&view, 0.45f, true);
        Mtx_RotateY(&view, -camYaw, true);
        Mtx_Translate(&view, -px, -(py + 1.0f), -pz, true);

        C3D_Mtx mvPlayer = view;
        Mtx_Translate(&mvPlayer, px, py, pz, true);
        Mtx_RotateY(&mvPlayer, face, true);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(g_ctx.tgt, C3D_CLEAR_ALL, 0x78A8F2FF, 0);
        C3D_FrameDrawOn(g_ctx.tgt);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_ctx.locProj, &g_ctx.proj);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_ctx.locMv, &view);
        gpumodel_draw(g_groundA, g_ctx, kGrassA);
        gpumodel_draw(g_groundB, g_ctx, kGrassB);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_ctx.locMv, &mvPlayer);
        gpumodel_draw(g_player, g_ctx, kPlayerTint);
        C3D_FrameEnd(0);
    }

    gpumodel_free(&g_player);
    gpumodel_free(&g_groundA);
    gpumodel_free(&g_groundB);
    gpuctx_free(&g_ctx);
    return nullptr;
}
