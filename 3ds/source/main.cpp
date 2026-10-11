// Petari 3DS bring-up: runs a few decomp math routines on-device and prints the results next to the expected values.
// Only <3ds.h>/<stdio.h> here; all Petari code lives in petari_test.cpp (see the note there).
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "j3d.h"
#include "soft3d.h"
#include "gpuview.h"
#include "rarc.h"
#include "sdfs.h"
#include "yaz0.h"

extern "C" int petari_run_tests(float* out);

static void row(const char* name, const float* v, int count, const char* expect) {
    printf("%-14s", name);
    for (int i = 0; i < count; i++) {
        printf(" %.3f", v[i]);
    }
    printf("\n   expect: %s\n", expect);
}

static char printable(uint8_t c) {
    return (c >= 32 && c < 127) ? (char)c : '.';
}

// Bottom screen: check the extracted game files on the SD card and the big-endian read helper.
static void sdTest() {
    static const char* const kFolders[] = {"ObjectData", "StageData", "LayoutData", "ParticleData",
                                           "MapPartsData", "SystemData", "ModuleData", "UsEnglish"};
    printf("SD game files\nsdmc:/petari/files\n\n");
    int found = 0;
    for (const char* name : kFolders) {
        int n = sdfs_count(name);
        if (n >= 0) {
            found++;
            printf("%-13s %d entries\n", name, n);
        } else {
            printf("%-13s MISSING\n", name);
        }
    }

    char rel[256];
    long size = 0;
    if (found > 0 && sdfs_find_first_file("StageData", rel, sizeof(rel), &size)) {
        uint8_t head[8];
        printf("\nFirst StageData file:\n %.38s\n size %ld\n", rel, size);
        if (sdfs_read_head(rel, head, sizeof(head))) {
            printf(" magic \"%c%c%c%c\"  BE32@4 = %lu\n", printable(head[0]), printable(head[1]), printable(head[2]),
                   printable(head[3]), (unsigned long)sdfs_be32(head + 4));
            printf(" (Yaz0 or RARC + a plausible size\n  means the byte order is right)\n");
        } else {
            printf(" could not read it\n");
        }
    } else if (found == 0) {
        printf("\nNo game folders found. Copy them\nto sd:/petari/files/\n");
    } else {
        printf("\nNo file found in StageData\n");
    }
    printf("\n[A] next page\n");
}

// Bottom screen page 2: decompress that first StageData archive and list what is inside it.
static void archiveTest() {
    char rel[256];
    long fileSize = 0;
    printf("Archive test\n\n");
    if (!sdfs_find_first_file("StageData", rel, sizeof(rel), &fileSize)) {
        printf("No StageData file found\n\n[A] next page\n");
        return;
    }
    printf("%.38s\n", rel);

    size_t rawSize = 0;
    uint8_t* raw = sdfs_read_all(rel, &rawSize);
    if (!raw) {
        printf("Could not read the file\n\n[A] next page\n");
        return;
    }

    uint8_t* arc = raw;
    size_t arcSize = rawSize;
    uint32_t unpacked = yaz0_size(raw, rawSize);
    if (unpacked) {
        arc = (uint8_t*)malloc(unpacked);
        if (!arc || !yaz0_decode(raw, rawSize, arc, unpacked)) {
            printf("Yaz0 decode FAILED\n\n[A] next page\n");
            free(arc);
            free(raw);
            return;
        }
        arcSize = unpacked;
        printf("Yaz0: %lu -> %lu bytes\n", (unsigned long)rawSize, (unsigned long)unpacked);
    } else {
        printf("Not Yaz0 (%lu bytes)\n", (unsigned long)rawSize);
    }

    Rarc r;
    if (rarc_open(&r, arc, arcSize)) {
        int files = 0;
        for (uint32_t i = 0; i < r.nodeCount; i++) {
            RarcNode n;
            if (rarc_node(r, i, &n) && !n.isDir) {
                files++;
            }
        }
        printf("RARC ok: %d files, %lu nodes\n\n", files, (unsigned long)r.nodeCount);
        int shown = 0;
        for (uint32_t i = 0; i < r.nodeCount && shown < 14; i++) {
            RarcNode n;
            if (rarc_node(r, i, &n) && !n.isDir) {
                printf("%-26.26s %7lu\n", n.name, (unsigned long)n.dataSize);
                shown++;
            }
        }
        if (files > shown) {
            printf("... and %d more\n", files - shown);
        }
    } else {
        printf("RARC header check FAILED\n");
        printf("first bytes: %02X %02X %02X %02X\n", arc[0], arc[1], arc[2], arc[3]);
    }
    printf("\n[A] next page\n");

    if (arc != raw) {
        free(arc);
    }
    free(raw);
}

static bool endsWith(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

// Loads an archive from the game folder (decompressing Yaz0 if needed). Caller free()s *outBuf.
static bool loadArchive(const char* rel, uint8_t** outBuf, size_t* outSize) {
    size_t rawSize = 0;
    uint8_t* raw = sdfs_read_all(rel, &rawSize);
    if (!raw) {
        return false;
    }
    uint32_t unpacked = yaz0_size(raw, rawSize);
    if (!unpacked) {
        *outBuf = raw;
        *outSize = rawSize;
        return true;
    }
    uint8_t* arc = (uint8_t*)malloc(unpacked);
    bool ok = arc && yaz0_decode(raw, rawSize, arc, unpacked);
    free(raw);
    if (!ok) {
        free(arc);
        return false;
    }
    *outBuf = arc;
    *outSize = unpacked;
    return true;
}

static int g_modelIdx = -1;
static J3dMesh g_mesh;  // mesh of the model currently shown (triCount 0 = none)
static bool g_haveMesh = false;  // index into ObjectData of the archive currently shown

// Bottom screen page 3: find a model (.bdl/.bmd) in an ObjectData archive and print what is inside it.
// dir = +1 shows the next archive that has a model, -1 the previous one.
static void modelTest(int dir) {
    printf("Model inspector\n\n");
    int total = sdfs_count("ObjectData");
    if (total <= 0) {
        printf("No ObjectData folder\n\n[A] next page\n");
        return;
    }
    int idx = g_modelIdx;
    for (int tried = 0; tried < total; tried++) {
        idx = (idx + dir + total) % total;
        char name[128];
        if (!sdfs_nth("ObjectData", idx, name, sizeof(name)) || !endsWith(name, ".arc")) {
            continue;
        }
        char rel[200];
        snprintf(rel, sizeof(rel), "ObjectData/%s", name);
        uint8_t* arc = nullptr;
        size_t arcSize = 0;
        if (!loadArchive(rel, &arc, &arcSize)) {
            continue;
        }
        Rarc r;
        bool shown = false;
        if (rarc_open(&r, arc, arcSize)) {
            for (uint32_t i = 0; i < r.nodeCount && !shown; i++) {
                RarcNode n;
                if (!rarc_node(r, i, &n) || n.isDir || !(endsWith(n.name, ".bdl") || endsWith(n.name, ".bmd"))) {
                    continue;
                }
                uint64_t off = (uint64_t)r.fileDataBase + n.dataOffset;
                if (off + n.dataSize > arcSize) {
                    continue;
                }
                J3dInfo info;
                if (!j3d_inspect(arc + off, n.dataSize, &info)) {
                    continue;
                }
                g_modelIdx = idx;
                shown = true;
                if (g_haveMesh) {
                    j3d_free_mesh(&g_mesh);
                }
                g_haveMesh = j3d_build_mesh(arc + off, n.dataSize, &g_mesh);
                printf("%.38s\n", name);
                printf("%.30s  %lu bytes\n", n.name, (unsigned long)n.dataSize);
                printf("type %s, %lu chunks\n\n", info.type, (unsigned long)info.chunkCount);
                printf("vertices ~%lu (%s%lu)\n", (unsigned long)info.posCount, info.posType == 4 ? "float x" : "int x",
                       (unsigned long)info.posComps);
                printf("shapes    %lu\n", (unsigned long)info.shapeCount);
                printf("packets   %lu\n", (unsigned long)info.packets);
                printf("triangles %lu\n", (unsigned long)info.triangles);
                printf("materials %lu\n", (unsigned long)info.materialCount);
                printf("textures  %lu\n", (unsigned long)info.textureCount);
                if (info.unsupportedAttr) {
                    printf("(some shapes use a layout I\n can't read yet)\n");
                }
            }
        }
        free(arc);
        if (shown) {
            printf("\n#%d of %d  [</>] other\n", idx + 1, total);
            printf(g_haveMesh ? "[X] view 3D  [Y] GPU view  [A] next\n" : "(no drawable mesh)  [A] next page\n");
            return;
        }
    }
    printf("No model found\n\n[A] next page\n");
}

static void printTop() {
    float r[32];
    petari_run_tests(r);

    printf("Petari 3DS bring-up\n\n");
    row("FastSqrt(2)", r + 0, 1, "~1.414 (exact 1.4142)");
    row("FastSqrt(-1)", r + 1, 1, "-1.000 (passthrough)");
    row("VECLerp .25", r + 2, 3, "2.500 5.000 7.500");
    row("VECScaleAdd 2", r + 5, 3, "20.000 40.000 60.000");
    row("QuatLerp .5", r + 8, 4, "0.000 0.500 0.000 0.500");
    row("Hermite", r + 12, 1, "0.625 (my decode of the PPC asm)");
    row("ApplyScale", r + 13, 3, "10.000 15.000 20.000");

    printf("\nPress START to exit.\n");
}

// Draws the current mesh to the top screen framebuffer with the software renderer (400x240, BGR8, rotated 90 degrees).
static void drawViewer(Soft3d& sf, float yaw, float pitch, float zoom) {
    soft3d_clear(&sf, 0x101828);
    soft3d_draw_mesh(&sf, g_mesh, yaw, pitch, zoom);
    u16 fw, fh;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 400; x++) {
            uint32_t c = sf.color[y * 400 + x];
            u8* p = fb + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] = c & 0xFF;
            p[1] = (c >> 8) & 0xFF;
            p[2] = (c >> 16) & 0xFF;
        }
    }
}

int main() {
    gfxInitDefault();
    PrintConsole top, bottom;
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    consoleSelect(&bottom);
    sdTest();
    consoleSelect(&top);

    printTop();


    int page = 0;
    bool viewing = false;
    Soft3d sf;
    bool sfOk = soft3d_init(&sf, 400, 240);
    float yaw = 0, pitch = 0.3f, zoom = 1.0f;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        if (viewing) {
            u32 held = hidKeysHeld();
            if (down & KEY_B) {
                viewing = false;
                gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);  // the text console needs its 16-bit format back
                consoleSelect(&top);
                consoleClear();
                printTop();
            } else {
                if (held & KEY_DLEFT) yaw -= 0.05f; else if (held & KEY_DRIGHT) yaw += 0.05f; else yaw += 0.015f;
                if (held & KEY_DUP) pitch += 0.04f;
                if (held & KEY_DDOWN) pitch -= 0.04f;
                if (held & KEY_R) zoom *= 1.03f;
                if (held & KEY_L) zoom /= 1.03f;
                drawViewer(sf, yaw, pitch, zoom);
            }
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
            continue;
        }
        if ((down & KEY_Y) && page == 2 && g_haveMesh) {
            const char* err = gpuview_run(g_mesh);
            consoleInit(GFX_TOP, &top);  // citro3d took over the top screen; bring the text console back
            consoleSelect(&top);
            printTop();
            if (err) {
                consoleSelect(&bottom);
                printf("\nGPU view failed: %s\n", err);
                consoleSelect(&top);
            }
            continue;
        }
        if ((down & KEY_X) && page == 2 && g_haveMesh && sfOk) {
            viewing = true;
            gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);  // the console switched the screen to 16-bit; the viewer writes 24-bit
            yaw = 0; pitch = 0.3f; zoom = 1.0f;
            continue;
        }
        int modelDir = 0;
        if ((down & KEY_DRIGHT) && page == 2) {
            modelDir = 1;
        } else if ((down & KEY_DLEFT) && page == 2) {
            modelDir = -1;
        }
        if ((down & KEY_A) || modelDir) {
            if (down & KEY_A) {
                page = (page + 1) % 3;
            }
            consoleSelect(&bottom);
            consoleClear();
            if (page == 1) {
                archiveTest();
            } else if (page == 2) {
                modelTest(modelDir ? modelDir : 1);
            } else {
                sdTest();
            }
            consoleSelect(&top);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (g_haveMesh) j3d_free_mesh(&g_mesh);
    if (sfOk) soft3d_free(&sf);
    gfxExit();
    return 0;
}
