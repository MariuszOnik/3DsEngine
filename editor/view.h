// The editor's camera: looking down at a point on the ground from above and behind it (as the game's RTS camera),
// orbiting round it. The point is where the brush works.
#pragma once
#include "cell.h"

typedef struct {
	float tx, ty;       // the point looked at, cell-relative
	float yaw, pitch;   // radians; pitch < 0 looks down
	float dist;         // from the point
	float eye[3];       // computed by viewUpdate: cell-relative
	float fwd[3];
} View;

#define VIEW_PITCH_MIN -1.48f   // almost straight down
#define VIEW_PITCH_MAX -0.12f   // almost level

void viewInit(View* v);
// A drag on the touch screen (pixels): sideways turns, up / down tilts.
void viewDrag(View* v, float dxPixels, float dyPixels);
// Moves by the circle pad (screen-relative, faster when zoomed out), turns, zooms; keeps the eye over the ground.
void viewUpdate(View* v, const ECell* c, float padX, float padY, float turn, float zoom, float dt);
