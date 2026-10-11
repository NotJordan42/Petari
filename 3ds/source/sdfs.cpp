#include "sdfs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char* kRoot = "sdmc:/petari/files";

static void joinPath(char* out, size_t n, const char* a, const char* b) {
    snprintf(out, n, "%s/%s", a, b);
}

int sdfs_count(const char* relDir) {
    char path[256];
    joinPath(path, sizeof(path), kRoot, relDir);
    DIR* d = opendir(path);
    if (!d) {
        return -1;
    }
    int count = 0;
    while (dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(d);
    return count;
}

static bool findFirst(const char* relDir, int depth, char* outRel, size_t outN, long* outSize) {
    char abs[256];
    joinPath(abs, sizeof(abs), kRoot, relDir);
    DIR* d = opendir(abs);
    if (!d) {
        return false;
    }
    bool found = false;
    while (!found) {
        dirent* e = readdir(d);
        if (!e) {
            break;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char rel[256];
        joinPath(rel, sizeof(rel), relDir, e->d_name);
        char full[320];
        joinPath(full, sizeof(full), kRoot, rel);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            snprintf(outRel, outN, "%s", rel);
            *outSize = (long)st.st_size;
            found = true;
        } else if (S_ISDIR(st.st_mode) && depth < 3) {
            found = findFirst(rel, depth + 1, outRel, outN, outSize);
        }
    }
    closedir(d);
    return found;
}

bool sdfs_find_first_file(const char* relDir, char* outRel, size_t outN, long* outSize) {
    return findFirst(relDir, 0, outRel, outN, outSize);
}

bool sdfs_read_head(const char* rel, uint8_t* buf, size_t n) {
    char path[320];
    joinPath(path, sizeof(path), kRoot, rel);
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}
