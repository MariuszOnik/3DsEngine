#include "view.h"

#include <math.h>

void viewInit(View* v)
{
	v->tx = v->ty = 0;
	v->yaw = 0;
	v->pitch = -0.95f;
	v->dist = 3500.0f;
}

void viewDrag(View* v, float dxPixels, float dyPixels)
{
	v->yaw += dxPixels * 0.012f;
	v->pitch -= dyPixels * 0.008f;   // drag down: look more from above
	if (v->pitch < VIEW_PITCH_MIN) v->pitch = VIEW_PITCH_MIN;
	if (v->pitch > VIEW_PITCH_MAX) v->pitch = VIEW_PITCH_MAX;
}

void viewUpdate(View* v, const ECell* c, float padX, float padY, float turn, float zoom, float dt)
{
	v->yaw += turn * 1.6f * dt;
	v->dist *= expf(zoom * 1.2f * dt);
	if (v->dist < 400) v->dist = 400;
	if (v->dist > 14000) v->dist = 14000;
	float sp = v->dist * 0.8f * dt;
	float s = sinf(v->yaw), co = cosf(v->yaw);
	v->tx += (co * padX + s * padY) * sp;
	v->ty += (-s * padX + co * padY) * sp;
	const float lim = CELL_HALF + 1500.0f;
	if (v->tx < -lim) v->tx = -lim;
	if (v->tx > lim) v->tx = lim;
	if (v->ty < -lim) v->ty = -lim;
	if (v->ty > lim) v->ty = lim;

	float gz = cellHeightAt(c, v->tx, v->ty);
	if (gz < 0) gz = 0;   // over the sea: its surface
	v->fwd[0] = cosf(v->pitch) * s;
	v->fwd[1] = cosf(v->pitch) * co;
	v->fwd[2] = sinf(v->pitch);
	v->eye[0] = v->tx - v->fwd[0] * v->dist;
	v->eye[1] = v->ty - v->fwd[1] * v->dist;
	v->eye[2] = gz - v->fwd[2] * v->dist;
	float under = cellHeightAt(c, v->eye[0], v->eye[1]) + 60.0f;
	if (v->eye[2] < under) v->eye[2] = under;
}
