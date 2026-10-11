// Tiny software triangle renderer used to look at decoded models while the GPU path doesn't exist yet.
// Pure C++ (no 3DS headers) so it can be tested on a PC. Orthographic camera, flat shading, z-buffered.
#pragma once
#include <stdint.h>

#include "j3d.h"

struct Soft3d {
    int w = 0, h = 0;
    uint32_t* color = nullptr;  // 0x00RRGGBB, row-major, y = 0 at the top
    float* depth = nullptr;
};

bool soft3d_init(Soft3d* s, int w, int h);
void soft3d_free(Soft3d* s);
void soft3d_clear(Soft3d* s, uint32_t rgb);

// Draws the mesh centred and scaled to fit. yaw/pitch in radians; zoom 1.0 fits the model to ~90% of the screen.
void soft3d_draw_mesh(Soft3d* s, const J3dMesh& mesh, float yaw, float pitch, float zoom);
