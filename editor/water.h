// The editor's sea: clear water as in the game (source/main.c drawWater). The bottom takes the water's colour with
// depth in terrain.v.pica; this adds the surface: gentle ripples (two scrolled layers of one ripple texture) and the
// sky mirrored at a glancing angle, faded out where the water meets the shore.
#pragma once
#include "cell.h"

bool waterInit(void);
// Draws the surface over the cell (after the land; blending and depth state are set here and left changed).
// mvp: projection x view, eye: the camera (cell-relative), t: seconds (the ripples move).
void waterDraw(const ECell* c, const C3D_Mtx* mvp, const float eye[3], float t);
