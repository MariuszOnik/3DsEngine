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
#define CELL_DIRTY (CELL_TEX / 16)   // painting's blocks per image side
#define CELL_SMALL (CELL_TEX / 8)
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
	// painting: which 16 x 16 pixel blocks of the image still wait to be encoded into tex again (a time budget per
	// frame, round robin from `nextDirty` so a held brush does not starve the rest)
	u8 dirty[CELL_DIRTY * CELL_DIRTY];
	u8* small;                          // the image at 1/8 (CELL_SMALL^2 RGB): mip levels 3.. are encoded from it
	int ndirty, nextDirty;
	u16 vtex[16 * 16];                  // the texture of each 512-unit square (land texture index + 1, 0 = none;
	                                    // row 0 = south), kept for the .esm's VTEX
} ECell;

// A new empty cell at (gx, gy): flat at NEW_LAND_Z, a grass-like image (until painting brings the land textures).
bool cellNew(ECell* c, int gx, int gy);

// New heights for the whole cell (CELL_GRID^2, e.g. read from an .esp); vertices and light follow.
void cellSetHeights(ECell* c, const float* h);

// Painting: blends the 64 x 64 texture `tex` (one repeat per 512 units, row 0 = north) into the image round
// (cx, cy), opacity up to `amount` at the centre with a soft rim; the squares mostly under the brush take `texIndex`
// (+ 1) in vtex. The GPU copy follows through cellFlushPaint.
void cellPaint(ECell* c, const u8* tex, int texIndex, float cx, float cy, float radius, float amount);

// Encodes waiting blocks into the GPU texture for up to `budgetMs` (all of them when budgetMs <= 0).
void cellFlushPaint(ECell* c, float budgetMs);

// The whole image again: from the GPU texture (ETC1, all levels as cellNew made it), or filled from vtex with the
// land textures (getTex(i) gives texture i's 64 x 64 pixels, NULL if none: the generated grass stays there).
bool cellSetImage(ECell* c, const u8* etc1, size_t size);
void cellImageFromVtex(ECell* c, const u8* (*getTex)(int i));
size_t cellImageSize(void);   // bytes of the ETC1 texture with all levels

// Height at a cell-relative point (bilinear; outside the cell: its nearest edge).
float cellHeightAt(const ECell* c, float x, float y);

// After heights changed in grid rows/columns [x0, x1] x [y0, y1]: vertices and their light again (the
// neighbours' too: the light depends on the slope), then the vertex buffer flushed for the GPU.
void cellHeightsChanged(ECell* c, int x0, int y0, int x1, int y1);
