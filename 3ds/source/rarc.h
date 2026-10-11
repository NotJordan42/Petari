// Minimal read-only RARC archive reader (the .arc container used throughout Super Mario Galaxy). All fields are big-endian.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct Rarc {
    const uint8_t* data;
    size_t size;
    uint32_t fileDataBase;  // absolute offset of the file data block
    uint32_t dirCount, dirOff;
    uint32_t nodeCount, nodeOff;
    uint32_t strOff, strSize;
};

struct RarcNode {
    const char* name;      // points into the archive's string table
    bool isDir;
    uint16_t id;
    uint32_t dataOffset;   // files: offset from fileDataBase; directories: directory index
    uint32_t dataSize;
};

// Validates the header and table bounds. The archive data must stay alive while the Rarc is used.
bool rarc_open(Rarc* r, const uint8_t* data, size_t size);

// Reads node `index` (a file or a directory entry) from the file-node table.
bool rarc_node(const Rarc& r, uint32_t index, RarcNode* out);
