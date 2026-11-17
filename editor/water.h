// The editor's sea: clear water as in the game (source/main.c drawWater). The bottom takes the water's colour with
// depth in terrain.v.pica; this adds the surface: gentle ripples (two scrolled layers of one ripple texture) that
// only light and shade the bottom (transparent, no added light), faded out where the water meets the shore.
#pragma once
#include "cell.h"

bool waterInit(void);
void waterSetRipples(float k);   // 0..1: how strongly the ripples light and shade the bottom
// Draws the surface over the cell (after the land; blending and depth state are set here and left changed).
// mvp: projection x view, eye: the camera (cell-relative), t: seconds (the ripples move).
void waterDraw(const ECell* c, const C3D_Mtx* mvp, const float eye[3], float t);
