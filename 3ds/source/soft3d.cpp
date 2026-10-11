#include "soft3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool soft3d_init(Soft3d* s, int w, int h) {
    s->w = w;
    s->h = h;
    s->color = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
    s->depth = (float*)malloc((size_t)w * h * sizeof(float));
    if (!s->color || !s->depth) {
        soft3d_free(s);
        return false;
    }
    return true;
}

void soft3d_free(Soft3d* s) {
    free(s->color);
    free(s->depth);
    s->color = nullptr;
    s->depth = nullptr;
    s->w = s->h = 0;
}

void soft3d_clear(Soft3d* s, uint32_t rgb) {
    size_t n = (size_t)s->w * s->h;
    for (size_t i = 0; i < n; i++) {
        s->color[i] = rgb;
        s->depth[i] = -1e30f;
    }
}

void soft3d_draw_mesh(Soft3d* s, const J3dMesh& mesh, float yaw, float pitch, float zoom) {
    if (!mesh.triCount) {
        return;
    }
    float cx = 0.5f * (mesh.minv[0] + mesh.maxv[0]);
    float cy = 0.5f * (mesh.minv[1] + mesh.maxv[1]);
    float cz = 0.5f * (mesh.minv[2] + mesh.maxv[2]);
    float ex = mesh.maxv[0] - mesh.minv[0];
    float ey = mesh.maxv[1] - mesh.minv[1];
    float ez = mesh.maxv[2] - mesh.minv[2];
    float radius = 0.5f * sqrtf(ex * ex + ey * ey + ez * ez);
    if (radius < 1e-6f) {
        radius = 1.0f;
    }
    float scale = zoom * 0.45f * (float)(s->w < s->h ? s->w : s->h) / radius;
    float sy = sinf(yaw), cyw = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);

    // light direction (towards the light), roughly from the upper right front
    const float lx = 0.35f, ly = 0.55f, lz = 0.75f;
    const float ll = sqrtf(lx * lx + ly * ly + lz * lz);

    for (uint32_t t = 0; t < mesh.triCount; t++) {
        const float* v = mesh.tris + (size_t)t * 9;
        float px[3], py[3], pz[3], rx[3], ry[3], rz[3];
        for (int i = 0; i < 3; i++) {
            float x = v[i * 3] - cx, y = v[i * 3 + 1] - cy, z = v[i * 3 + 2] - cz;
            float x1 = x * cyw + z * sy;   // yaw about Y
            float z1 = -x * sy + z * cyw;
            float y2 = y * cp - z1 * sp;   // pitch about X
            float z2 = y * sp + z1 * cp;
            rx[i] = x1;
            ry[i] = y2;
            rz[i] = z2;
            px[i] = 0.5f * s->w + x1 * scale;
            py[i] = 0.5f * s->h - y2 * scale;
            pz[i] = z2;  // larger = closer to the camera
        }
        // flat shading from the face normal (two-sided, so triangle winding doesn't matter)
        float ax = rx[1] - rx[0], ay = ry[1] - ry[0], az = rz[1] - rz[0];
        float bx = rx[2] - rx[0], by = ry[2] - ry[0], bz = rz[2] - rz[0];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-12f) {
            continue;
        }
        float lit = fabsf((nx * lx + ny * ly + nz * lz) / (nl * ll));
        float shade = 0.25f + 0.75f * lit;
        uint32_t r = (uint32_t)(shade * 150.0f), g = (uint32_t)(shade * 190.0f), b = (uint32_t)(shade * 255.0f);
        uint32_t rgb = (r << 16) | (g << 8) | b;

        float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (fabsf(area) < 1e-6f) {
            continue;
        }
        int minx = (int)floorf(fminf(px[0], fminf(px[1], px[2])));
        int maxx = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
        int miny = (int)floorf(fminf(py[0], fminf(py[1], py[2])));
        int maxy = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx > s->w - 1) maxx = s->w - 1;
        if (maxy > s->h - 1) maxy = s->h - 1;
        float inv = 1.0f / area;
        for (int y = miny; y <= maxy; y++) {
            for (int x = minx; x <= maxx; x++) {
                float fx = x + 0.5f, fy = y + 0.5f;
                float w0 = ((px[1] - fx) * (py[2] - fy) - (px[2] - fx) * (py[1] - fy)) * inv;
                float w1 = ((px[2] - fx) * (py[0] - fy) - (px[0] - fx) * (py[2] - fy)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
                float z = w0 * pz[0] + w1 * pz[1] + w2 * pz[2];
                size_t i = (size_t)y * s->w + x;
                if (z > s->depth[i]) {
                    s->depth[i] = z;
                    s->color[i] = rgb;
                }
            }
        }
    }
}
