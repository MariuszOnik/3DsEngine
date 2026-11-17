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
	if (!c->rgb || !work || !C3D_TexInitMipmap(&c->tex, CELL_TEX, CELL_TEX, GPU_ETC1)) return false;
	grassImage(c->rgb);
	etc1EncodeMips(c->rgb, CELL_TEX, CELL_TEX, c->tex.data, work, 0);
	linearFree(work);
	C3D_TexFlush(&c->tex);
	C3D_TexSetFilter(&c->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(&c->tex, GPU_LINEAR);
	C3D_TexSetWrap(&c->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return true;
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
