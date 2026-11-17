#include "cell.h"

#include <math.h>
#include <string.h>

#include "../source/etc1.h"

// The sun as tools/mwbake.py lights the terrain: colour = base * (AMBIENT + SUNCOL * max(0, n . SUN)).
static const float SUN[3] = { -0.35f, 0.45f, 0.82f };
static const float AMBIENT[3] = { 0.52f, 0.54f, 0.60f };
static const float SUNCOL[3] = { 0.62f, 0.58f, 0.50f };

static void lightVertex(ECell* c, int x, int y)
{
	static float sun[3];
	if (sun[2] == 0) {
		float l = sqrtf(SUN[0] * SUN[0] + SUN[1] * SUN[1] + SUN[2] * SUN[2]);
		for (int i = 0; i < 3; i++) sun[i] = SUN[i] / l;
	}
	const int G = CELL_GRID;
	int xl = x > 0 ? x - 1 : x, xr = x < G - 1 ? x + 1 : x;
	int yd = y > 0 ? y - 1 : y, yu = y < G - 1 ? y + 1 : y;
	float dx = (c->h[y * G + xr] - c->h[y * G + xl]) / ((xr - xl) * CELL_STEP);
	float dy = (c->h[yu * G + x] - c->h[yd * G + x]) / ((yu - yd) * CELL_STEP);
	float nx = -dx, ny = -dy, nz = 1.0f;
	float d = (nx * sun[0] + ny * sun[1] + nz * sun[2]) / sqrtf(nx * nx + ny * ny + nz * nz);
	if (d < 0) d = 0;
	Vtx* v = &c->v[y * G + x];
	for (int ch = 0; ch < 3; ch++) {
		float k = AMBIENT[ch] + SUNCOL[ch] * d;
		v->c[ch] = (u8)((k > 1 ? 1 : k) * 255.0f);
	}
	v->c[3] = 255;
	float z = c->h[y * G + x];
	v->p[2] = (s16)(z < -32000 ? -32000 : z > 32000 ? 32000 : z);
}

void cellHeightsChanged(ECell* c, int x0, int y0, int x1, int y1)
{
	x0--; y0--; x1++; y1++;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > CELL_GRID - 1) x1 = CELL_GRID - 1;
	if (y1 > CELL_GRID - 1) y1 = CELL_GRID - 1;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) lightVertex(c, x, y);
	GSPGPU_FlushDataCache(c->v, CELL_GRID * CELL_GRID * sizeof(Vtx));
}

static void smallUpdate(ECell* c, int x0, int y0, int x1, int y1);
static void previewUpdate(ECell* c, int x0, int y0, int x1, int y1);

// ---- a grass-like image (value noise in three octaves, tiling)

static u32 hash2(int x, int y)
{
	u32 h = (u32)x * 374761393u + (u32)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static float valueNoise(float x, float y, int period)
{
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = ((xi % period) + period) % period, y0 = ((yi % period) + period) % period;
	int x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
	float a = (hash2(x0, y0) & 0xFFFF) / 65535.0f, b = (hash2(x1, y0) & 0xFFFF) / 65535.0f;
	float cc = (hash2(x0, y1) & 0xFFFF) / 65535.0f, d = (hash2(x1, y1) & 0xFFFF) / 65535.0f;
	float top = a + (b - a) * fx, bottom = cc + (d - cc) * fx;
	return top + (bottom - top) * fy;
}

static void grassImage(u8* rgb)
{
	const int S = CELL_TEX;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			float n = valueNoise(x / 64.0f, y / 64.0f, S / 64) * 0.5f + valueNoise(x / 16.0f, y / 16.0f, S / 16) * 0.3f +
			          valueNoise(x / 4.0f, y / 4.0f, S / 4) * 0.2f;
			u8* o = rgb + ((size_t)y * S + x) * 3;
			o[0] = (u8)(62 + 50 * n);
			o[1] = (u8)(84 + 52 * n);
			o[2] = (u8)(38 + 26 * n);
		}
}

// ---- the cell

bool cellNew(ECell* c, int gx, int gy)
{
	const int G = CELL_GRID;
	memset(c, 0, sizeof *c);
	c->gx = gx;
	c->gy = gy;
	for (int i = 0; i < G * G; i++) c->h[i] = NEW_LAND_Z;
	c->v = linearAlloc(G * G * sizeof(Vtx));
	c->nidx = (G - 1) * (G - 1) * 6;
	c->idx = linearAlloc(c->nidx * sizeof(u16));
	if (!c->v || !c->idx) return false;
	for (int y = 0; y < G; y++)
		for (int x = 0; x < G; x++) {
			Vtx* v = &c->v[y * G + x];
			v->p[0] = (s16)(x * CELL_STEP - CELL_HALF);
			v->p[1] = (s16)(y * CELL_STEP - CELL_HALF);
			v->p[3] = 0;
			v->t[0] = (s16)(x * 1024 / (G - 1));   // uv * 1024: the image over the whole cell
			v->t[1] = (s16)(y * 1024 / (G - 1));
		}
	u16* ix = c->idx;
	for (int y = 0; y < G - 1; y++)
		for (int x = 0; x < G - 1; x++) {   // as the baker: (a, b, d), (a, d, c)
			u16 a = y * G + x, b = a + 1, cc = a + G, d = cc + 1;
			*ix++ = a; *ix++ = b; *ix++ = d;
			*ix++ = a; *ix++ = d; *ix++ = cc;
		}
	GSPGPU_FlushDataCache(c->idx, c->nidx * sizeof(u16));
	cellHeightsChanged(c, 0, 0, G - 1, G - 1);
	BufInfo_Init(&c->buf);
	BufInfo_Add(&c->buf, c->v, sizeof(Vtx), 3, 0x210);

	// the terrain image, encoded on the console in the game's ETC1 layout
	c->rgb = linearAlloc((size_t)CELL_TEX * CELL_TEX * 3);
	u8* work = linearAlloc((size_t)CELL_TEX * CELL_TEX * 3 / 4);
	c->small = linearAlloc((size_t)CELL_SMALL * CELL_SMALL * 3);
	if (!c->rgb || !c->small || !work || !C3D_TexInitMipmap(&c->tex, CELL_TEX, CELL_TEX, GPU_ETC1)) return false;
	if (C3D_TexInit(&c->preview, CELL_TEX, CELL_TEX, GPU_RGB565)) {   // without it: ETC1 while painting (slower)
		C3D_TexSetFilter(&c->preview, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&c->preview, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	grassImage(c->rgb);
	smallUpdate(c, 0, 0, CELL_SMALL, CELL_SMALL);
	previewUpdate(c, 0, 0, CELL_TEX - 1, CELL_TEX - 1);
	etc1EncodeMips(c->rgb, CELL_TEX, CELL_TEX, c->tex.data, work, 0);
	linearFree(work);
	C3D_TexFlush(&c->tex);
	C3D_TexSetFilter(&c->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(&c->tex, GPU_LINEAR);
	C3D_TexSetWrap(&c->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return true;
}

// ---- painting

#define PX_UNITS (2 * CELL_HALF / CELL_TEX)   // 8 units a pixel
#define SMALL_LEVEL 3                          // CELL_SMALL = CELL_TEX >> SMALL_LEVEL
#define SMALL_OFFSET ((CELL_TEX * CELL_TEX + CELL_TEX * CELL_TEX / 4 + CELL_TEX * CELL_TEX / 16) / 2)   // its ETC1 bytes

// The 1/8 copy over small pixels [x0, x1) x [y0, y1): each the average of 8 x 8 image pixels.
static void smallUpdate(ECell* c, int x0, int y0, int x1, int y1)
{
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			int sum[3] = { 0, 0, 0 };
			for (int yy = 0; yy < 8; yy++) {
				const u8* p = c->rgb + ((size_t)(y * 8 + yy) * CELL_TEX + x * 8) * 3;
				for (int xx = 0; xx < 8; xx++, p += 3) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; }
			}
			u8* o = c->small + ((size_t)y * CELL_SMALL + x) * 3;
			for (int ch = 0; ch < 3; ch++) o[ch] = (u8)((sum[ch] + 32) >> 6);
		}
}

static u32 morton8(u32 x, u32 y)
{
	return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

// The uncompressed copy over image pixels [x0, x1] x [y0, y1] (GPU order: 8 x 8 tiles from the bottom row up).
static void previewUpdate(ECell* c, int x0, int y0, int x1, int y1)
{
	if (!c->preview.data) return;
	u16* d = c->preview.data;
	const int S = CELL_TEX;
	for (int y = y0; y <= y1; y++) {
		int ty = S - 1 - y;
		u16* tileRow = d + (size_t)(ty >> 3) * (S >> 3) * 64;
		const u8* p = c->rgb + ((size_t)y * S + x0) * 3;
		for (int x = x0; x <= x1; x++, p += 3)
			tileRow[(x >> 3) * 64 + morton8(x & 7, ty & 7)] = (u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
	}
	int tr0 = (S - 1 - y1) >> 3, tr1 = (S - 1 - y0) >> 3;   // the tile rows written
	GSPGPU_FlushDataCache(d + (size_t)tr0 * (S >> 3) * 64, (size_t)(tr1 - tr0 + 1) * (S >> 3) * 64 * 2);
}

// A tiling noise for the brush's rim (fixed to the ground, so strokes meet without seams)
#define NOISE 128
static u8 rimNoise[NOISE * NOISE];

static void makeRimNoise(void)
{
	static float grid[3][17 * 17];
	const int cells[3] = { 4, 8, 16 };
	const float amp[3] = { 0.5f, 0.3f, 0.2f };
	u32 seed = 12345;
	for (int o = 0; o < 3; o++)
		for (int i = 0; i < 17 * 17; i++) { seed = seed * 1664525u + 1013904223u; grid[o][i] = (seed >> 8 & 0xFFFF) / 65535.0f; }
	for (int y = 0; y < NOISE; y++)
		for (int x = 0; x < NOISE; x++) {
			float v = 0;
			for (int o = 0; o < 3; o++) {
				int n = cells[o];
				float fx = x * n / (float)NOISE, fy = y * n / (float)NOISE;
				int ix = (int)fx, iy = (int)fy;
				fx -= ix; fy -= iy;
				fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
				int x1 = (ix + 1) % n, y1 = (iy + 1) % n;
				float a = grid[o][iy * 17 + ix], b = grid[o][iy * 17 + x1], cc = grid[o][y1 * 17 + ix], d = grid[o][y1 * 17 + x1];
				v += amp[o] * ((a + (b - a) * fx) * (1 - fy) + (cc + (d - cc) * fx) * fy);
			}
			rimNoise[y * NOISE + x] = (u8)(v * 255);
		}
}

static void markDirty(ECell* c, int px0, int py0, int px1, int py1)
{
	for (int by = py0 >> 4; by <= py1 >> 4; by++)
		for (int bx = px0 >> 4; bx <= px1 >> 4; bx++) {
			u8* d = &c->dirty[by * CELL_DIRTY + bx];
			if (!*d) { *d = 1; c->ndirty++; }
		}
}

void cellPaint(ECell* c, const u8* tex, int texIndex, float cx, float cy, float radius, float amount)
{
	const int S = CELL_TEX;
	int px0 = (int)floorf((cx - radius + CELL_HALF) / PX_UNITS), px1 = (int)ceilf((cx + radius + CELL_HALF) / PX_UNITS);
	int py0 = (int)floorf((CELL_HALF - cy - radius) / PX_UNITS), py1 = (int)ceilf((CELL_HALF - cy + radius) / PX_UNITS);
	if (px0 < 0) px0 = 0;
	if (py0 < 0) py0 = 0;
	if (px1 > S - 1) px1 = S - 1;
	if (py1 > S - 1) py1 = S - 1;
	if (px0 > px1 || py0 > py1 || amount <= 0) return;
	if (amount > 1) amount = 1;
	if (!rimNoise[0] && !rimNoise[1]) makeRimNoise();
	float inv = 1.0f / (radius * radius);
	for (int py = py0; py <= py1; py++) {
		float wy = CELL_HALF - (py + 0.5f) * PX_UNITS - cy;
		const u8* trow = tex + (py & (64 - 1)) * 64 * 3;
		u8* o = c->rgb + ((size_t)py * S + px0) * 3;
		for (int px = px0; px <= px1; px++, o += 3) {
			float wx = (px + 0.5f) * PX_UNITS - CELL_HALF - cx;
			float d2 = (wx * wx + wy * wy) * inv;
			if (d2 >= 1) continue;
			// a ragged rim: where the noise and the texture's own light parts are high the texture comes through
			// first (stones before the soil between them), so the edge is a transition, not a circle
			const u8* t = trow + (px & (64 - 1)) * 3;
			float n = rimNoise[(py & (NOISE - 1)) * NOISE + (px & (NOISE - 1))] * (0.6f / 255) +
			          (t[0] + 2 * t[1] + t[2]) * (0.4f / 1020);
			float w = (1 - d2) * 1.6f - 0.35f - n * 0.7f;
			if (w <= 0) continue;
			if (w > 1) w = 1;
			w = w * w * (3 - 2 * w);
			int a = (int)(amount * w * 256 + 0.5f);
			if (a <= 0) continue;
			for (int k = 0; k < 3; k++) {
				int diff = t[k] - o[k];
				int step = (diff * a + (diff > 0 ? 255 : -255)) / 256;   // rounded away from 0: it gets there
				if ((diff > 0 && step > diff) || (diff < 0 && step < diff)) step = diff;
				o[k] = (u8)(o[k] + step);
			}
		}
	}
	markDirty(c, px0, py0, px1, py1);
	previewUpdate(c, px0, py0, px1, py1);
	if (c->preview.data) c->previewOn = true;

	// the .esm's table: the squares whose middle is well inside the brush
	for (int sy = 0; sy < 16; sy++)
		for (int sx = 0; sx < 16; sx++) {
			float mx = (sx + 0.5f) * 512 - CELL_HALF - cx, my = (sy + 0.5f) * 512 - CELL_HALF - cy;
			if ((mx * mx + my * my) * inv < 0.36f) c->vtex[sy * 16 + sx] = (u16)(texIndex + 1);
		}
}

void cellFlushPaint(ECell* c, float budgetMs)
{
	if (!c->ndirty) { c->previewOn = false; return; }
	const int N = CELL_DIRTY * CELL_DIRTY;
	u64 t0 = svcGetSystemTick();
	bool any = false;
	for (int k = 0; k < N && c->ndirty; k++) {
		int i = (c->nextDirty + k) % N;
		if (!c->dirty[i]) continue;
		c->dirty[i] = 0;
		c->ndirty--;
		int bx = i % CELL_DIRTY, by = i / CELL_DIRTY;
		// levels 0-2 from the image, the smaller ones from the 1/8 copy (from the image they cost ~100x more)
		etc1UpdateRectLevels(c->rgb, CELL_TEX, CELL_TEX, bx * 16, by * 16, bx * 16 + 16, by * 16 + 16, c->tex.data, 0,
		                     SMALL_LEVEL);
		smallUpdate(c, bx * 2, by * 2, bx * 2 + 2, by * 2 + 2);
		etc1UpdateRect(c->small, CELL_SMALL, CELL_SMALL, bx * 2, by * 2, bx * 2 + 2, by * 2 + 2,
		               (u8*)c->tex.data + SMALL_OFFSET, 0);
		any = true;
		if (budgetMs > 0 && (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC > budgetMs) {
			c->nextDirty = (i + 1) % N;
			break;
		}
	}
	if (any) C3D_TexFlush(&c->tex);
	if (!c->ndirty) c->previewOn = false;
}

size_t cellImageSize(void)
{
	int levels;
	return etc1Size(CELL_TEX, CELL_TEX, &levels);
}

bool cellSetImage(ECell* c, const u8* etc1, size_t size)
{
	if (size != cellImageSize()) return false;
	memcpy(c->tex.data, etc1, size);
	C3D_TexFlush(&c->tex);
	etc1Decode(etc1, CELL_TEX, CELL_TEX, c->rgb);
	smallUpdate(c, 0, 0, CELL_SMALL, CELL_SMALL);
	previewUpdate(c, 0, 0, CELL_TEX - 1, CELL_TEX - 1);
	c->previewOn = false;
	memset(c->dirty, 0, sizeof c->dirty);
	c->ndirty = 0;
	return true;
}

void cellImageFromVtex(ECell* c, const u8* (*getTex)(int i))
{
	const int S = CELL_TEX;
	bool any = false;
	for (int sy = 0; sy < 16; sy++)
		for (int sx = 0; sx < 16; sx++) {
			int v = c->vtex[sy * 16 + sx];
			const u8* t = v ? getTex(v - 1) : NULL;
			if (!t) continue;
			any = true;
			int py0 = (15 - sy) * 64;   // the image's row 0 is north; vtex row 0 south
			for (int y = 0; y < 64; y++) memcpy(c->rgb + ((size_t)(py0 + y) * S + sx * 64) * 3, t + y * 64 * 3, 64 * 3);
		}
	if (!any) return;
	u8* work = linearAlloc((size_t)S * S * 3 / 4);
	if (!work) return;
	etc1EncodeMips(c->rgb, S, S, c->tex.data, work, 0);
	linearFree(work);
	C3D_TexFlush(&c->tex);
	smallUpdate(c, 0, 0, CELL_SMALL, CELL_SMALL);
	previewUpdate(c, 0, 0, CELL_TEX - 1, CELL_TEX - 1);
	c->previewOn = false;
	memset(c->dirty, 0, sizeof c->dirty);
	c->ndirty = 0;
}

void cellSetHeights(ECell* c, const float* h)
{
	memcpy(c->h, h, sizeof c->h);
	cellHeightsChanged(c, 0, 0, CELL_GRID - 1, CELL_GRID - 1);
}

float cellHeightAt(const ECell* c, float x, float y)
{
	const int G = CELL_GRID;
	float fx = (x + CELL_HALF) / CELL_STEP, fy = (y + CELL_HALF) / CELL_STEP;
	if (fx < 0) fx = 0;
	if (fy < 0) fy = 0;
	if (fx > G - 1.001f) fx = G - 1.001f;
	if (fy > G - 1.001f) fy = G - 1.001f;
	int ix = (int)fx, iy = (int)fy;
	fx -= ix;
	fy -= iy;
	const float* h = c->h + iy * G + ix;
	return (h[0] * (1 - fx) + h[1] * fx) * (1 - fy) + (h[G] * (1 - fx) + h[G + 1] * fx) * fy;
}
