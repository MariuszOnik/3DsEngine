// One cell being edited: the game's terrain formats (tools/mwbake.py, source/main.c).
// A 65 x 65 height grid every 128 units (row 0 = the south edge), vertices relative to the cell's centre with the
// sun baked into their colours, a 1024^2 terrain image (RGB here, row 0 = north; ETC1 on the GPU).
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

#define CELL_GRID 65
#define CELL_STEP 128.0f
#define CELL_HALF 4096.0f
#define CELL_TEX 1024
#define NEW_LAND_Z -256.0f   // a new cell: flat land under the sea, as in the Construction Set

typedef struct { s16 p[4]; s16 t[2]; u8 c[4]; } Vtx;   // the game's vertex (16 bytes)

typedef struct {
	int gx, gy;                         // place on the world grid (the centre at gx * 8192 + 4096)
	float h[CELL_GRID * CELL_GRID];     // heights
	Vtx* v;                             // CELL_GRID^2 vertices, linear memory
	u16* idx;
	int nidx;
	C3D_BufInfo buf;
	C3D_Tex tex;                        // the terrain image on the GPU (ETC1, all mip levels)
	u8* rgb;                            // the same image uncompressed, kept for painting
} ECell;

// A new empty cell at (gx, gy): flat at NEW_LAND_Z, a grass-like image (until painting brings the land textures).
bool cellNew(ECell* c, int gx, int gy);

// Height at a cell-relative point (bilinear; outside the cell: its nearest edge).
float cellHeightAt(const ECell* c, float x, float y);

// After heights changed in grid rows/columns [x0, x1] x [y0, y1]: vertices and their light again (the
// neighbours' too: the light depends on the slope), then the vertex buffer flushed for the GPU.
void cellHeightsChanged(ECell* c, int x0, int y0, int x1, int y1);
