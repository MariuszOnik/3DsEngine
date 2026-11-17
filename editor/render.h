// Drawing for the editor: one shader (editor/terrain.v.pica, the game's vertex format), the cell's land, the sea
// over it and the brush's rim on the ground.
#pragma once
#include "cell.h"
#include "view.h"

bool renderInit(void);   // after C3D_Init
// The brush's rim (a band on the ground) and a cross at its centre, for this frame.
void renderSetBrush(const ECell* c, float cx, float cy, float radius);
void renderScene(const ECell* c, const View* v, bool water);
