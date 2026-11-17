// The terrain brushes. A brush works round a cell-relative point with a radius; its strength falls off smoothly
// to the rim (half a cosine). Each call is one frame's worth (dt seconds).
#pragma once
#include "cell.h"

typedef enum { TOOL_NONE, TOOL_RAISE, TOOL_LOWER, TOOL_SMOOTH, TOOL_FLATTEN, TOOL_COUNT } Tool;

typedef struct {
	float radius;     // units
	float rate;       // raise / lower: units a second at the centre
	float flatZ;      // flatten: the height it works towards (taken under the brush when the tool starts)
} Brush;

const char* toolName(Tool t);   // Polish, for the bottom screen

// Applies `tool` round (cx, cy); returns false when nothing of the cell was under it.
bool brushApply(ECell* c, Tool tool, const Brush* b, float cx, float cy, float dt);
