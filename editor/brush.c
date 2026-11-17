#include "brush.h"

#include <math.h>
#include <string.h>

const char* toolName(Tool t)
{
	static const char* const names[TOOL_COUNT] = { "-", "podnies", "obniz", "wygladz", "wyrownaj" };
	return t >= 0 && t < TOOL_COUNT ? names[t] : "?";
}

bool brushApply(ECell* c, Tool tool, const Brush* b, float cx, float cy, float dt)
{
	const int G = CELL_GRID;
	const float r = b->radius;
	int x0 = (int)floorf((cx - r + CELL_HALF) / CELL_STEP), x1 = (int)ceilf((cx + r + CELL_HALF) / CELL_STEP);
	int y0 = (int)floorf((cy - r + CELL_HALF) / CELL_STEP), y1 = (int)ceilf((cy + r + CELL_HALF) / CELL_STEP);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > G - 1) x1 = G - 1;
	if (y1 > G - 1) y1 = G - 1;
	if (tool == TOOL_NONE || x0 > x1 || y0 > y1) return false;

	static float before[CELL_GRID * CELL_GRID];   // smoothing reads the heights as they were this frame
	if (tool == TOOL_SMOOTH) memcpy(before, c->h, sizeof before);
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			float px = x * CELL_STEP - CELL_HALF - cx, py = y * CELL_STEP - CELL_HALF - cy;
			float d = sqrtf(px * px + py * py) / r;
			if (d >= 1) continue;
			float w = 0.5f + 0.5f * cosf(d * 3.14159265f);
			float* h = &c->h[y * G + x];
			switch (tool) {
			case TOOL_RAISE: *h += b->rate * w * dt; break;
			case TOOL_LOWER: *h -= b->rate * w * dt; break;
			default: {
				float target = b->flatZ;
				if (tool == TOOL_SMOOTH) {
					int xl = x > 0 ? x - 1 : x, xr = x < G - 1 ? x + 1 : x;
					int yd = y > 0 ? y - 1 : y, yu = y < G - 1 ? y + 1 : y;
					target = (before[y * G + xl] + before[y * G + xr] + before[yd * G + x] + before[yu * G + x]) * 0.25f;
				}
				float k = w * dt * 4.0f;   // a quarter of a second to get there at the centre
				*h += (target - *h) * (k > 1 ? 1 : k);
			}
			}
			if (*h > 30000) *h = 30000;
			if (*h < -30000) *h = -30000;
		}
	cellHeightsChanged(c, x0, y0, x1, y1);
	return true;
}
