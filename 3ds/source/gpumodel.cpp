#include "gpumodel.h"

#include <math.h>
#include <stdlib.h>
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
    float u, v;
};

// Converts a row-major RGBA image into the PICA200's layout: 8x8 tiles, Morton order inside each tile.
static void swizzle(const uint32_t* src, int w, int h, uint32_t* dst) {
    int tilesX = w / 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int tile = (y >> 3) * tilesX + (x >> 3);
            int xi = x & 7, yi = y & 7;
            int m = (xi & 1) | ((yi & 1) << 1) | ((xi & 2) << 1) | ((yi & 2) << 2) | ((xi & 4) << 2) | ((yi & 4) << 3);
            dst[tile * 64 + m] = src[y * w + x];
        }
    }
}

static GPU_TEXTURE_WRAP_PARAM wrapMode(uint8_t w) {
    return w == 0 ? GPU_CLAMP_TO_EDGE : (w == 2 ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
}

static bool pow2(int v) {
    return v >= 8 && v <= 1024 && (v & (v - 1)) == 0;
}

const char* gpuctx_init(GpuCtx* c) {
    memset(c, 0, sizeof(*c));
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSet3D(false);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return "C3D_Init failed";
    }
    c->tgt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!c->tgt) {
        C3D_Fini();
        return "render target failed";
    }
    C3D_RenderTargetSetOutput(c->tgt, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    c->dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    if (!c->dvlb) {
        C3D_RenderTargetDelete(c->tgt);
        C3D_Fini();
        return "shader parse failed";
    }
    shaderProgramInit(&c->prog);
    shaderProgramSetVsh(&c->prog, &c->dvlb->DVLE[0]);
    C3D_BindProgram(&c->prog);
    c->locProj = shaderInstanceGetUniformLocation(c->prog.vertexShader, "projection");
    c->locMv = shaderInstanceGetUniformLocation(c->prog.vertexShader, "modelview");
    c->locTint = shaderInstanceGetUniformLocation(c->prog.vertexShader, "tint");

    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(true, GPU_GREATER, 0x08);  // skip fully transparent texels
    Mtx_PerspTilt(&c->proj, C3D_AngleFromDegrees(40.0f), C3D_AspectRatioTop, 0.1f, 200.0f, false);
    return nullptr;
}

void gpuctx_free(GpuCtx* c) {
    shaderProgramFree(&c->prog);
    DVLB_Free(c->dvlb);
    C3D_RenderTargetDelete(c->tgt);
    C3D_Fini();
    memset(c, 0, sizeof(*c));
}

const char* gpumodel_create(GpuModel* m, const J3dMesh& mesh, const float center[3], float scale) {
    memset(m, 0, sizeof(*m));
    if (!mesh.triCount) {
        return "no mesh";
    }
    size_t vcount = (size_t)mesh.triCount * 3;
    Vtx* vbo = (Vtx*)linearAlloc(vcount * sizeof(Vtx));
    if (!vbo) {
        return "out of linear memory";
    }
    m->vbo = vbo;

    // Triangles are grouped by texture so each group is one draw call. Group 0 = untextured, group k+1 = texture k.
    m->groups = mesh.texCount + 1;
    if (m->groups > 33) {
        m->groups = 33;
    }
    for (uint32_t t = 0; t < mesh.triCount; t++) {
        int g = mesh.triTex[t] + 1;
        if (g < 0 || (uint32_t)g >= m->groups) g = 0;
        m->groupCount[g]++;
    }
    for (uint32_t k = 1; k <= m->groups; k++) {
        m->groupStart[k] = m->groupStart[k - 1] + m->groupCount[k - 1];
    }
    uint32_t fill[34] = {0};

    for (uint32_t t = 0; t < mesh.triCount; t++) {
        const float* v = mesh.tris + (size_t)t * 9;
        const float* uv = mesh.uvs + (size_t)t * 6;
        int g = mesh.triTex[t] + 1;
        if (g < 0 || (uint32_t)g >= m->groups) g = 0;
        Vtx* o = vbo + (size_t)(m->groupStart[g] + fill[g]++) * 3;
        for (int i = 0; i < 3; i++) {
            o[i].x = (v[i * 3] - center[0]) * scale;
            o[i].y = (v[i * 3 + 1] - center[1]) * scale;
            o[i].z = (v[i * 3 + 2] - center[2]) * scale;
            o[i].u = uv[i * 2];
            o[i].v = uv[i * 2 + 1];
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

    // Upload textures. Anything that isn't a power of two between 8 and 1024 draws untextured.
    m->texCount = mesh.texCount < 32 ? mesh.texCount : 32;
    for (uint32_t k = 0; k < m->texCount; k++) {
        const J3dTexture& jt = mesh.textures[k];
        if (!pow2(jt.w) || !pow2(jt.h)) {
            continue;
        }
        uint32_t* tiled = (uint32_t*)malloc((size_t)jt.w * jt.h * 4);
        if (!tiled) {
            continue;
        }
        swizzle(jt.rgba, jt.w, jt.h, tiled);
        if (C3D_TexInit(&m->tex[k], jt.w, jt.h, GPU_RGBA8)) {
            C3D_TexLoadImage(&m->tex[k], tiled, GPU_TEXFACE_2D, 0);
            C3D_TexSetFilter(&m->tex[k], GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&m->tex[k], wrapMode(jt.wrapS), wrapMode(jt.wrapT));
            m->texOk[k] = true;
        }
        free(tiled);
    }
    return nullptr;
}

void gpumodel_free(GpuModel* m) {
    for (uint32_t k = 0; k < m->texCount; k++) {
        if (m->texOk[k]) C3D_TexDelete(&m->tex[k]);
    }
    if (m->vbo) linearFree(m->vbo);
    memset(m, 0, sizeof(*m));
}

void gpumodel_draw(const GpuModel& m, const GpuCtx& c, const float untexTint[4]) {
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, m.vbo, sizeof(Vtx), 3, 0x210);
    for (uint32_t k = 0; k < m.groups; k++) {
        if (!m.groupCount[k]) {
            continue;
        }
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        if (k > 0 && m.texOk[k - 1]) {
            // textured groups are alpha blended and don't write depth, so see-through parts don't hide what is behind
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                           GPU_ONE_MINUS_SRC_ALPHA);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
            C3D_TexBind(0, (C3D_Tex*)&m.tex[k - 1]);
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, c.locTint, 1.0f, 1.0f, 1.0f, 1.0f);
        } else {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0, (GPU_TEVSRC)0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, c.locTint, untexTint[0], untexTint[1], untexTint[2], untexTint[3]);
        }
        C3D_DrawArrays(GPU_TRIANGLES, (int)(m.groupStart[k] * 3), (int)(m.groupCount[k] * 3));
    }
}
