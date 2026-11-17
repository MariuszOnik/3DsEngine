#include "water.h"

#include <math.h>
#include <stdlib.h>

#include "water_shbin.h"

static DVLB_s* dvlb;
static shaderProgram_s prog;
static int uMvp, uOrigin, uUvA, uUvB, uRefl;
static C3D_AttrInfo attr;
static C3D_Tex ripple;
static bool rippleOk;
static Vtx* grid;
static C3D_BufInfo buf;

// ---- the ripple texture (as the game's makeRipples, without the glints' alpha): L8 256, tiling

static float lattice(int x, int y, int gx, int gy, u32 seed)
{
	x &= gx - 1; y &= gy - 1;
	u32 h = (u32)x * 374761393u + (u32)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (h & 0xFFFF) / 65535.0f;
}

static float vnoise(float x, float y, int gx, int gy, u32 seed)
{
	int ix = (int)floorf(x), iy = (int)floorf(y);
	float fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
	float a = lattice(ix, iy, gx, gy, seed), b = lattice(ix + 1, iy, gx, gy, seed);
	float c = lattice(ix, iy + 1, gx, gy, seed), d = lattice(ix + 1, iy + 1, gx, gy, seed);
	return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static void fbm(float* out, int N, int gx0, int gy0, int octaves, float gain, u32 seed)
{
	float lo = 1e9f, hi = -1e9f;
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			float v = 0, amp = 1;
			int gx = gx0, gy = gy0;
			for (int o = 0; o < octaves; o++) {
				v += amp * vnoise(x * (float)gx / N, y * (float)gy / N, gx, gy, seed + o * 101);
				amp *= gain; gx *= 2; gy *= 2;
			}
			out[y * N + x] = v;
			if (v < lo) lo = v;
			if (v > hi) hi = v;
		}
	for (int i = 0; i < N * N; i++) out[i] = (out[i] - lo) / (hi - lo);
}

static u32 morton8(int x, int y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static void makeRipples(void)
{
	const int N = 256;
	float* L = malloc(N * N * sizeof(float));
	if (!L || !C3D_TexInitMipmap(&ripple, N, N, GPU_L8)) { free(L); return; }
	// crests of stretched noise (1 - |2n - 1|: lines where the noise crosses its middle), low contrast round grey
	fbm(L, N, 8, 4, 4, 0.55f, 4242);
	float mean = 0;
	for (int i = 0; i < N * N; i++) { L[i] = 1 - fabsf(2 * L[i] - 1); mean += L[i]; }
	mean /= N * N;
	for (int i = 0; i < N * N; i++) L[i] = 0.5f + (L[i] - mean) * 0.22f;
	u8* dst = ripple.data;
	for (int w = N;;) {   // every level, averaged down in place
		for (int y = 0; y < w; y++)
			for (int x = 0; x < w; x++) {
				int l = (int)(L[y * w + x] * 255.0f + 0.5f);
				dst[((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7)] = (u8)(l < 0 ? 0 : l > 255 ? 255 : l);
			}
		dst += w * w;
		if (w <= 8) break;
		int h = w / 2;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < h; x++) {
				int i0 = 2 * y * w + 2 * x;
				L[y * h + x] = (L[i0] + L[i0 + 1] + L[i0 + w] + L[i0 + w + 1]) * 0.25f;
			}
		w = h;
	}
	free(L);
	C3D_TexFlush(&ripple);
	C3D_TexSetFilter(&ripple, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(&ripple, GPU_LINEAR);
	C3D_TexSetWrap(&ripple, GPU_REPEAT, GPU_REPEAT);
	rippleOk = true;
}

bool waterInit(void)
{
	dvlb = DVLB_ParseFile((u32*)water_shbin, water_shbin_size);
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	uMvp = shaderInstanceGetUniformLocation(prog.vertexShader, "mvp");
	uOrigin = shaderInstanceGetUniformLocation(prog.vertexShader, "origin");
	uUvA = shaderInstanceGetUniformLocation(prog.vertexShader, "uvA");
	uUvB = shaderInstanceGetUniformLocation(prog.vertexShader, "uvB");
	uRefl = shaderInstanceGetUniformLocation(prog.vertexShader, "refl");
	AttrInfo_Init(&attr);
	AttrInfo_AddLoader(&attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(&attr, 1, GPU_SHORT, 2);
	AttrInfo_AddLoader(&attr, 2, GPU_UNSIGNED_BYTE, 4);
	grid = linearAlloc(CELL_GRID * CELL_GRID * sizeof(Vtx));
	if (!grid) return false;
	for (int y = 0; y < CELL_GRID; y++)
		for (int x = 0; x < CELL_GRID; x++) {
			Vtx* v = &grid[y * CELL_GRID + x];
			v->p[0] = (s16)(x * CELL_STEP - CELL_HALF);
			v->p[1] = (s16)(y * CELL_STEP - CELL_HALF);
			v->p[2] = v->p[3] = 0;
			v->t[0] = v->t[1] = 0;
		}
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, grid, sizeof(Vtx), 3, 0x210);
	makeRipples();
	return true;
}

void waterDraw(const ECell* c, const C3D_Mtx* mvp, const float eye[3], float t)
{
	if (!grid || !rippleOk) return;
	// the light scattered back (a faint cool grey, the game's "colourless" water) and the shore's fade: the surface
	// is drawn where the land is under the sea, fading out in the last 40 units (no hard edge, as in the game)
	for (int i = 0; i < CELL_GRID * CELL_GRID; i++) {
		float d = -c->h[i];
		Vtx* v = &grid[i];
		v->c[0] = 6; v->c[1] = 8; v->c[2] = 9;
		v->c[3] = (u8)(d <= 0 ? 0 : d < 40 ? d / 40.0f * 255 : 255);
	}
	GSPGPU_FlushDataCache(grid, CELL_GRID * CELL_GRID * sizeof(Vtx));

	C3D_BindProgram(&prog);
	C3D_SetAttrInfo(&attr);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, mvp);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uOrigin, -eye[0], -eye[1], -eye[2], 0);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uUvA, fmodf(t * 0.031f, 1.0f), fmodf(t * 0.019f, 1.0f), 14.0f / 8192.0f, 0);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uUvB, fmodf(-t * 0.023f, 1.0f), fmodf(t * 0.034f, 1.0f), 11.0f / 8192.0f, 0);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uRefl, 0.16f, 0.20f, 0.24f, 0);   // the editor's sky, toned down
	C3D_TexBind(1, &ripple);
	C3D_TexBind(2, &ripple);
	// the two layers added signed (mid grey: nothing), x the colour x 4, added to the picture x the shore's fade
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_TEXTURE2, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_4);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, c->nidx, C3D_UNSIGNED_SHORT, c->idx);   // the land's own triangle list
	C3D_TexEnvInit(C3D_GetTexEnv(1));
}
