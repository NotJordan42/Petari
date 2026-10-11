// A tiny "world": a checkerboard ground and the player's model you can walk around and jump with.
#pragma once
#include "j3d.h"

// Blocks until B is pressed. Returns nullptr on success, or an error message. The caller restores the text console.
const char* playground_run(const J3dMesh& player);
