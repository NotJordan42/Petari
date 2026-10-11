// GPU (citro3d) model viewer for the top screen. Blocks until the user presses B.
#pragma once
#include "j3d.h"

// Returns nullptr on success, or a short error message. The caller must re-initialise the top-screen text console afterwards.
const char* gpuview_run(const J3dMesh& mesh);
