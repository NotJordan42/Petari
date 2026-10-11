#include "rarc.h"

#include <string.h>

#include "sdfs.h"

bool rarc_open(Rarc* r, const uint8_t* d, size_t size) {
    if (size < 0x40 || memcmp(d, "RARC", 4) != 0) {
        return false;
    }
    const uint32_t info = 0x20;  // info block follows the 0x20-byte header
    r->data = d;
    r->size = size;
    r->fileDataBase = 0x20 + sdfs_be32(d + 0x0C);
    r->dirCount = sdfs_be32(d + info + 0x00);
    r->dirOff = info + sdfs_be32(d + info + 0x04);
    r->nodeCount = sdfs_be32(d + info + 0x08);
    r->nodeOff = info + sdfs_be32(d + info + 0x0C);
    r->strSize = sdfs_be32(d + info + 0x10);
    r->strOff = info + sdfs_be32(d + info + 0x14);

    if ((uint64_t)r->dirOff + (uint64_t)r->dirCount * 0x10 > size) {
        return false;
    }
    if ((uint64_t)r->nodeOff + (uint64_t)r->nodeCount * 0x14 > size) {
        return false;
    }
    if ((uint64_t)r->strOff + (uint64_t)r->strSize > size) {
        return false;
    }
    return r->fileDataBase <= size;
}

bool rarc_node(const Rarc& r, uint32_t index, RarcNode* out) {
    if (index >= r.nodeCount) {
        return false;
    }
    const uint8_t* p = r.data + r.nodeOff + (size_t)index * 0x14;
    out->id = sdfs_be16(p);
    out->isDir = (p[4] & 0x02) != 0 || out->id == 0xFFFF;
    uint32_t nameOff = sdfs_be16(p + 6);
    out->name = nameOff < r.strSize ? (const char*)(r.data + r.strOff + nameOff) : "?";
    out->dataOffset = sdfs_be32(p + 8);
    out->dataSize = sdfs_be32(p + 0xC);
    return true;
}
