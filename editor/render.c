#include "render.h"

#include <math.h>

#include "terrain_shbin.h"
#include "water.h"

#define RING_SEG 48
#define RING_VERTS ((RING_SEG + 1) * 2 + 8)
#define RING_INDICES (RING_SEG * 6 + 12)

static DVLB_s* dvlb;
static shaderProgram_s prog;
static int uMvp, uOrigin, uWater;
static C3D_AttrInfo attr;

static Vtx* water;
static C3D_BufInfo waterBuf;
static u16* waterIdx;
static Vtx* ring;
static C3D_BufInfo ringBuf;
static u16* ringIdx;

static void setVtx(Vtx* v, float x, float y, float z, u8 r, u8 g, u8 b, u8 a)
{
	v->p[0] = (s16)x; v->p[1] = (s16)y; v->p[2] = (s16)z; v->p[3] = 0;
	v->t[0] = v->t[1] = 0;
	v->c[0] = r; v->c[1] = g; v->c[2] = b; v->c[3] = a;
}

bool renderInit(void)
{
	dvlb = DVLB_ParseFile((u32*)terrain_shbin, terrain_shbin_size);
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	uMvp = shaderInstanceGetUniformLocation(prog.vertexShader, "mvp");
	uOrigin = shaderInstanceGetUniformLocation(prog.vertexShader, "origin");
	uWater = shaderInstanceGetUniformLocation(prog.vertexShader, "water");
	AttrInfo_Init(&attr);
	AttrInfo_AddLoader(&attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(&attr, 1, GPU_SHORT, 2);
	AttrInfo_AddLoader(&attr, 2, GPU_UNSIGNED_BYTE, 4);

	water = linearAlloc(4 * sizeof(Vtx));
	waterIdx = linearAlloc(6 * sizeof(u16));
	ring = linearAlloc(RING_VERTS * sizeof(Vtx));
	ringIdx = linearAlloc(RING_INDICES * sizeof(u16));
	if (!water || !waterIdx || !ring || !ringIdx || !waterInit()) return false;

	// the sea: one quad over the whole cell at 0, a light clear sheen (the depth colour is on the land under it)
	const float h = CELL_HALF;
	setVtx(&water[0], -h, -h, 0, 170, 225, 235, 56);
	setVtx(&water[1], h, -h, 0, 170, 225, 235, 56);
	setVtx(&water[2], h, h, 0, 170, 225, 235, 56);
	setVtx(&water[3], -h, h, 0, 170, 225, 235, 56);
	const u16 q[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++) waterIdx[i] = q[i];
	BufInfo_Init(&waterBuf);
	BufInfo_Add(&waterBuf, water, sizeof(Vtx), 3, 0x210);

	// the rim: RING_SEG quads (inner, outer, next inner, next outer), then the cross's two quads
	for (int i = 0; i < RING_SEG; i++) {
		u16 a = i * 2;
		u16* o = &ringIdx[i * 6];
		o[0] = a; o[1] = a + 1; o[2] = a + 3;
		o[3] = a; o[4] = a + 3; o[5] = a + 2;
	}
	const u16 cross[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
	for (int i = 0; i < 12; i++) ringIdx[RING_SEG * 6 + i] = (RING_SEG + 1) * 2 + cross[i];
	BufInfo_Init(&ringBuf);
	BufInfo_Add(&ringBuf, ring, sizeof(Vtx), 3, 0x210);
	GSPGPU_FlushDataCache(water, 4 * sizeof(Vtx));
	GSPGPU_FlushDataCache(waterIdx, 6 * sizeof(u16));
	GSPGPU_FlushDataCache(ringIdx, RING_INDICES * sizeof(u16));
	return true;
}

void renderSetBrush(const ECell* c, float cx, float cy, float radius)
{
	float w = radius * 0.04f + 6.0f;
	for (int i = 0; i <= RING_SEG; i++) {
		float a = i * 2 * 3.14159265f / RING_SEG, s = sinf(a), co = cosf(a);
		for (int k = 0; k < 2; k++) {
			float rr = k ? radius + w : radius - w;
			float x = cx + co * rr, y = cy + s * rr;
			setVtx(&ring[i * 2 + k], x, y, cellHeightAt(c, x, y) + 6.0f, 255, 220, 60, 255);
		}
	}
	int base = (RING_SEG + 1) * 2;
	float z = cellHeightAt(c, cx, cy) + 8.0f, l = radius * 0.12f + 20.0f, t = w * 0.6f;
	const float q[8][2] = { { -l, -t }, { l, -t }, { l, t }, { -l, t }, { -t, -l }, { t, -l }, { t, l }, { -t, l } };
	for (int i = 0; i < 8; i++) setVtx(&ring[base + i], cx + q[i][0], cy + q[i][1], z, 255, 255, 255, 255);
	GSPGPU_FlushDataCache(ring, RING_VERTS * sizeof(Vtx));
}

static void texEnv(bool textured)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (textured) {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_PRIMARY_COLOR, 0);   // unit 1: unit 0 is citro2d's
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	} else {
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
}

void renderScene(const ECell* c, const View* v, bool showWater, float iod)
{
	C3D_Mtx view, proj, mvp;
	Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(v->fwd[0], v->fwd[1], v->fwd[2]), FVec3_New(0, 0, 1), false);
	if (iod != 0)   // the stereo 3D (as the game): the brush's point on the screen's plane
		Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 8.0f, v->dist * 3.0f + 12000.0f, iod,
		                    v->dist, false);
	else
		Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 8.0f, v->dist * 3.0f + 12000.0f, false);
	Mtx_Multiply(&mvp, &proj, &view);

	C3D_BindProgram(&prog);
	C3D_SetAttrInfo(&attr);
	for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));   // citro2d's stages (the touch screen) off
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, &mvp);
	// camera-relative positions, as in the game (everything here is relative to the cell's centre already)
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uOrigin, -v->eye[0], -v->eye[1], -v->eye[2], 1.0f / 1024.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uWater, 0.16f, 0.60f, 0.70f, 1.0f / 800.0f);   // turquoise by 800 units down
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

	// the land
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	texEnv(true);
	C3D_TexBind(1, (C3D_Tex*)(c->previewOn ? &c->preview : &c->tex));   // painting: the uncompressed copy
	C3D_SetBufInfo((C3D_BufInfo*)&c->buf);
	C3D_DrawElements(GPU_TRIANGLES, c->nidx, C3D_UNSIGNED_SHORT, c->idx);

	// the sea: the bottom took its colour above (terrain.v.pica); the surface adds the game's gentle ripples
	if (showWater) {
		waterDraw(c, &mvp, v->eye, svcGetSystemTick() / (float)SYSCLOCK_ARM11);
		C3D_BindProgram(&prog);   // back to the land's program and its uniforms (the water's share the registers)
		C3D_SetAttrInfo(&attr);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, &mvp);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uOrigin, -v->eye[0], -v->eye[1], -v->eye[2], 1.0f / 1024.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uWater, 0.16f, 0.60f, 0.70f, 1.0f / 800.0f);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	}
	texEnv(false);

	// the brush, always on top (no depth colour on it)
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uWater, 1, 1, 1, 0);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_SetBufInfo(&ringBuf);
	C3D_DrawElements(GPU_TRIANGLES, RING_INDICES, C3D_UNSIGNED_SHORT, ringIdx);
	// Leave the state citro2d expects (the touch screen is drawn after this): blending on, no depth test. With
	// blending off its glyphs (quads with an alpha mask) came out as solid blocks, on the console and in Azahar.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
	               GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
}
