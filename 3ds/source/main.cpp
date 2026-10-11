// Petari 3DS bring-up: runs a few decomp math routines on-device and prints the results next to the expected values.
// Only <3ds.h>/<stdio.h> here; all Petari code lives in petari_test.cpp (see the note there).
#include <3ds.h>
#include <stdio.h>

#include "sdfs.h"

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
}

int main() {
    gfxInitDefault();
    PrintConsole top, bottom;
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    consoleSelect(&bottom);
    sdTest();
    consoleSelect(&top);

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

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
