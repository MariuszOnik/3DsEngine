// 3dsOpenworld: a test of how much of Morrowind's open world the 3DS can carry.
//
// The PC baker (tools/mwbake.py) turns each exterior cell into one file: terrain with a
// composite texture and every static object pre-transformed and merged per material and
// sub-chunk, the light baked into the vertex colours. Here the cells are streamed from
// romfs around the camera and drawn with one shader; the bottom screen shows what it costs.
//
// Old 3DS profile (hardware test 1: GPU ~ 1 ms + 0.13 ms per 1000 triangles, CPU ~ 0.025 ms
// per draw): 3x3 cells in memory, 30 fps, and the draw distance set by a pacer from the
// averaged GPU time, so the world takes a fixed share of the frame (the rest is for the
// characters still to come).
//
// Streaming: a loader thread reads the cell files (the read waits on the SD, not the CPU);
// the main thread only allocates the buffer before and sets the cell up after (textures,
// buffers): a cell is drawn once it is whole, and an old one stays until it is 2 rings away.
//
// Controls (no C-stick needed): circle pad move, ABXY look, L/R down/up, D-pad up/down speed,
// D-pad left/right GPU budget (auto distance) or distance (manual), SELECT cell radius,
// touch left half: auto/manual distance, touch right half: 30/60 fps cap, START quit.

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "data.h"
#include "world_shbin.h"
#include "water_shbin.h"
#include "skin_shbin.h"
#include "obj_shbin.h"
#include "audio.h"
#include "ui.h"
#include "objects.h"
#include "script.h"

#define CELL 8192.0f
#define HALF 4096.0f
#define MAX_CELLS 64
#define MAX_AVAIL 1024
#define MAX_ROUTE 4096
#define LOD_DIST 3500.0f   // beyond it big things are drawn from their LOD copy (tools/simplify.py)

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define SKY_R 0x8C
#define SKY_G 0xA8
#define SKY_B 0xC4

typedef struct {
	char magic[4];
	s32 gx, gy;
	u16 nbatch, tsize;
	u32 ttofs, bofs, end;
	u32 colofs, ncol;   // MWC2: collision boxes (8 floats each: centre x y, axis cos sin, half sizes, z from, to);
	                    // MWC3: ncol = the offset of the blocked squares for one on stairs / roofs (0: as occ)
	u32 flofs, nfloor;  // MWC3: floors to walk on (bridges, docks, stairs): triangles, 9 floats each
	u32 occofs;         // MWC3: what blocks the way, 512 x 512 bits (16 units each, row by row), 0 none
} __attribute__((packed)) CellHdr;

typedef struct {
	u16 tex;
	u8 flags, opacity;   // flags: 1 alpha test, 2 two-sided, 4 blended (water)
	u32 vofs, vcount, iofs, icount;
	float c[3], r;
} __attribute__((packed)) BatchRec;

typedef struct { s16 p[4]; s16 t[2]; u8 c[4]; } Vtx;   // the baker's vertex (16 bytes)

typedef struct {
	bool used;
	int gx, gy;
	u8* data;
	u32 size;
	CellHdr* hdr;
	BatchRec* b;
	C3D_BufInfo* bufs;
	C3D_Tex ttex;
	bool hasT;
	u32 tris;
	float* hgt;   // 65 x 65 terrain heights (from the terrain batches), NULL without land
	const float* col;   // the collision boxes (cell-relative), ncol of them
	int ncol;
	const float* flo;   // the floor triangles (cell-relative), nflo of them
	int nflo;
	u16* floB[8][8];    // per 1024-unit square: the floor triangles over it (n, then the indices)
	const u8* occ;      // the blocked 16-unit squares (bits), NULL: the boxes instead
	const u8* occU;     // the same for one up on a building's stairs or roof (the walls under it do not block)
	Vtx* wv;      // the water grid (65 x 65, coloured by the depth), NULL without water
	Vtx* cv;      // the same grid laid on the bottom: the caustics' strength in the colour
	C3D_BufInfo cbuf;
	Vtx* tv;      // the water grid again: what of the bottom's colour comes through (multiplied)
	C3D_BufInfo tbuf;
	u16* wi;
	int wni;
	C3D_BufInfo wbuf;
} Cell;

typedef struct { int gx, gy; u32 size; } Avail;

// The memory split (after Gothic3DS): libctru gives the linear heap (what the GPU reads:
// cells and textures) at most 32 MB. This engine needs little ordinary heap, so it keeps
// 12 MB of that and gives the rest to the linear heap (Old 3DS 3dsx: 53 MB).
extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_linear_heap, __ctru_heap_size, __ctru_linear_heap_size;
void __system_allocateHeaps(void)
{
	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
	s64 maxCommit = 0, currentCommit = 0;
	ResourceLimitType type = RESLIMIT_COMMIT;
	svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
	svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
	svcCloseHandle(reslimit);
	u32 remaining = (u32)(maxCommit - currentCommit) & ~0xFFFu;
	u32 heap = 12u << 20;
	if (heap > remaining / 2) heap = (remaining / 2) & ~0xFFFu;
	__ctru_heap_size = heap;
	__ctru_linear_heap_size = remaining - heap;
	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC,
	                              (MemPerm)(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
	                              (MemPerm)(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = (char*)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

static C3D_Tex* texs;
static int ntex;
static u32 texBytes;
static Avail avail[MAX_AVAIL];
static int navail;
static Cell cells[MAX_CELLS];
static float route[MAX_ROUTE][3];
static int nroute;

static DVLB_s* dvlb;
static shaderProgram_s prog;
static int uMvp, uOrigin, uFog;
static C3D_AttrInfo attr;

static float eye[3], yaw, pitch;
static float vel[2];               // smoothed camera velocity (units/s), for predictive loading
static int radius = 1;
static float farDist = 8000.0f;
static float speed = 400.0f;

// draw distance pacer
static bool autoFar = true;
static float gpuBudget = 16.0f;    // ms of GPU per frame for the world (30 fps leaves ~17 ms for the rest)
static float paceGpu;
static int paceFrames, paceCooldown;

static bool cap30 = true;
static float fogStart = 0.45f;     // the fog starts at this share of the draw distance

// Looks (touch, bottom left in RTS mode): 0 effects (clear water by depth, caustics, soft
// shadow), 1 the same without the caustics, 2 calm (the water's colour alone, no ripples),
// 3 plain (the baker's flat water, as in T1-T6). The stereo 3D follows the 3D slider (each eye drawn: twice the GPU).
#define FX_CALM 2    // the clear water without ripples, glints or caustics
#define FX_PLAIN 3
static int fxMode = 0;
// The water's tint (D-pad left/right in RTS mode; bench.txt "blue"): the bottom's filter deep,
// the mirrored colour, the scattered light (all 0..255)
#define NTINT 3
static int waterTint = 0;
static bool tintChanged;
static const char* const TINT_NAMES[NTINT] = { "colourless", "blue      ", "olive     " };
static const u8 TINTS[NTINT][3][3] = {
	{ { 52, 58, 62 }, { 36, 40, 44 }, { 5, 6, 7 } },     // colourless (a hint of cool)
	{ { 38, 60, 80 }, { 28, 40, 56 }, { 4, 7, 10 } },    // delicate blue
	{ { 40, 66, 50 }, { 34, 50, 40 }, { 5, 8, 6 } },     // olive (the mountain-lake look)
};
static float fxTime;               // seconds, for the ripples
static C3D_Tex rippleTex, detailTex;
static bool rippleOk, detailOk;
static int uDetail;
static DVLB_s* wDvlb;
static shaderProgram_s wProg;
static int wMvp, wOrigin, wFog, wUvA, wUvB, wFres, wRefl;

// RTS camera (after the Switch port's tactical camera: -55 degrees, 700 units). The pitch and
// the distance are locked, because the ground the camera sees (and so the GPU cost) depends
// only on them; turning round the character keeps that cost, so it stays free.
#define RTS_PITCH (-55.0f * (float)M_PI / 180.0f)
#define RTS_DIST 700.0f     // (the first RTS test's distance; now the zoom's: rtsDist, D-pad up/down)
#define RTS_FAR 2600.0f     // the farthest corner of the view on flat ground is ~1800 away; the rest for tall things
#define RTS_TARGET_Z 70.0f  // the camera looks at the character's chest
#define RTS_LOAD 3500.0f    // RTS: cells nearer the character than this are loaded,
#define RTS_DROP 6500.0f    // ... farther than this unloaded (instead of the 3 x 3 rings)
#define WALK_SPEED 170.0f
#define RUN_SPEED 420.0f
static bool rtsMode = true;              // playing the character (whatever the camera); off: the free camera
// The character's cameras (D-pad left cycles): RTS from above, behind the back (TPP), its eyes (FPP).
enum { CAM_RTS, CAM_TPP, CAM_FPP, NCAM };
static int camMode = CAM_RTS;
static const char* const CAM_NAMES[NCAM] = { "RTS", "TPP", "FPP" };
static float rtsDist = 420.0f;           // zoom (D-pad up/down), RTS
static bool benchRtsFixed;               // the benchmark keeps the first test's angle
static float rtsPitch = RTS_PITCH;       // its angle (X + D-pad up/down): -20 .. -85 degrees
static float tppDist = 230.0f;           // ... and behind the back
static float camPitch;                   // TPP / FPP: looking up / down (FPP: D-pad up/down)
#define TPP_FAR 4500.0f
#define EYE_Z 118.0f                     // the eyes over the feet (FPP)
static bool underwater;                  // the camera is under the water's surface (the look changes)
static u32 fogColour = 0xFF000000 | SKY_B << 16 | SKY_G << 8 | SKY_R;
static float loadDist = 3500.0f, dropDist = 6500.0f;   // character modes: cells nearer than loadDist come in
// swimming: deeper than SWIM_DEPTH the character swims, its feet at SWIM_Z (head out); A dives
#define SWIM_DEPTH 105.0f
#define SWIM_Z (-100.0f)
static bool swimming;
static float chrVz;                      // falling (off a bridge, a dock), jumping
static bool chrAir;                      // off the ground (the jump animation)
static float chrPitch;                   // the body tipped in the water
// the torch (D-pad right): carried in the left hand, lights what is near (world shader), a flame
static bool torchOn;
static float torchPos[3];                // the flame, world space (from the hand bone)
static float torchFlicker = 1.0f;
static int uTorch, uTorchCol;
static float rtsYaw;                     // the camera's turn round the character
static float chr[3];                     // the character's feet
static float chrYaw, chrPhase, chrSwing; // facing; walk cycle phase and its strength (0 standing .. 1)
static float chrPlaceUp = 400.0f;      // where the floor is looked for when placing: from this over chr[2]
static bool chrPlaced;                   // on the ground once its cell is in

// stats
static int statDraws, statCulled, statSmall, statLod;   // statSmall: batches of small / under-water things left out
static u32 statTris;
static int loadsTotal, loadFailed;
static float lastIoMs, lastSetupMs;

static u64 ticks(void) { return svcGetSystemTick(); }
static float msSince(u64 t0) { return (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC; }

// ------------------------------------------------------------------ log (buffered: SD writes cost frames)

static char logBuf[128 * 1024];
static int logLen;
static FILE* logFile;

static void logw(const char* fmt, ...)
{
	if (!logFile) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(logBuf + logLen, sizeof logBuf - logLen, fmt, ap);
	va_end(ap);
	if (n > 0) logLen += n;
	if (logLen > (int)sizeof logBuf - 1024) logLen = sizeof logBuf - 1024;
}

static void logFlush(void)
{
	if (!logFile || !logLen) return;
	fwrite(logBuf, 1, logLen, logFile);
	fflush(logFile);
	logLen = 0;
}

// ------------------------------------------------------------------ files, textures

static u8* readFile(const char* path, u32* size)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* buf = (u8*)malloc(n);
	if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (size) *size = (u32)n;
	return buf;
}

static bool texFrom(C3D_Tex* t, int w, int h, int fmt, const u8* src, u32 bytes, bool repeat)
{
	// the baker's formats: 0 RGB565, 1 RGBA4, 2 ETC1 (4 bpp), 3 ETC1A4 (8 bpp)
	static const GPU_TEXCOLOR formats[4] = { GPU_RGB565, GPU_RGBA4, GPU_ETC1, GPU_ETC1A4 };
	if (fmt < 0 || fmt > 3) return false;
	if (!C3D_TexInitMipmap(t, w, h, formats[fmt])) return false;
	u32 total = C3D_TexCalcTotalSize(t->size, C3D_TexCalcMaxLevel(w, h));
	memcpy(t->data, src, bytes < total ? bytes : total);
	C3D_TexFlush(t);
	C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(t, GPU_LINEAR);
	GPU_TEXTURE_WRAP_PARAM wr = repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE;
	C3D_TexSetWrap(t, wr, wr);
	return true;
}

static bool loadTextures(void)
{
	u32 size;
	u8* buf = readFile(dataPath("textures.bin"), &size);
	if (!buf || memcmp(buf, "MWT1", 4)) return false;
	memcpy(&ntex, buf + 4, 4);
	texs = calloc(ntex, sizeof(C3D_Tex));
	u8* p = buf + 8;
	for (int i = 0; i < ntex; i++) {
		u16 w, h; u8 fmt; u32 bytes;
		memcpy(&w, p, 2); memcpy(&h, p + 2, 2); fmt = p[4]; memcpy(&bytes, p + 8, 4);
		p += 12;
		if (!texFrom(&texs[i], w, h, fmt, p, bytes, true)) printf("tex %d failed\n", i);
		texBytes += bytes;
		p += bytes;
	}
	free(buf);
	return true;
}

static bool loadIndex(void)
{
	u32 size;
	u8* buf = readFile(dataPath("index.bin"), &size);
	if (!buf || memcmp(buf, "MWI2", 4)) { free(buf); return false; }
	u8* p = buf + 4;
	memcpy(&navail, p, 4); p += 4;
	if (navail > MAX_AVAIL) navail = MAX_AVAIL;
	memcpy(avail, p, navail * 12); p += navail * 12;
	float st[4];
	memcpy(st, p, 16); p += 16;
	eye[0] = st[0]; eye[1] = st[1]; eye[2] = st[2]; yaw = st[3];
	memcpy(&nroute, p, 4); p += 4;
	if (nroute > MAX_ROUTE) nroute = MAX_ROUTE;
	memcpy(route, p, nroute * 12);
	free(buf);
	return true;
}

static const Avail* findAvail(int gx, int gy)
{
	for (int i = 0; i < navail; i++)
		if (avail[i].gx == gx && avail[i].gy == gy) return &avail[i];
	return NULL;
}

static Cell* findCell(int gx, int gy)
{
	for (int i = 0; i < MAX_CELLS; i++)
		if (cells[i].used && cells[i].gx == gx && cells[i].gy == gy) return &cells[i];
	return NULL;
}

// The water of a cell as a 65 x 65 grid over the terrain's: the colour and the transparency
// from the depth (clear and green-blue at the shore with a band of foam, dark and opaque in
// the deep), the squares wholly on land left out. Replaces the baker's flat quad (flag 4).
static void buildWater(Cell* c)
{
	c->wv = (Vtx*)linearAlloc(65 * 65 * sizeof(Vtx));
	c->wi = (u16*)linearAlloc(64 * 64 * 6 * sizeof(u16));
	if (!c->wv || !c->wi) {
		if (c->wv) linearFree(c->wv);
		if (c->wi) linearFree(c->wi);
		c->wv = NULL; c->wi = NULL;
		return;
	}
	c->cv = (Vtx*)linearAlloc(65 * 65 * sizeof(Vtx));   // (none: no caustics, the water still)
	c->tv = (Vtx*)linearAlloc(65 * 65 * sizeof(Vtx));   // (none: one layer only)
	float dep[65 * 65];
	for (int y = 0; y <= 64; y++)
		for (int x = 0; x <= 64; x++) {
			int k = y * 65 + x;
			float d = c->hgt ? -c->hgt[k] : 2000.0f;
			dep[k] = d;
			// Crystal water (the user, hardware test 9): not a see-through colour over the bottom (the
			// dark mud showed as a puddle) but two layers, like real water: the bottom's colour
			// filtered (tv, multiplied: red goes first, then green; almost nothing at the shore,
			// a deep blue far out) and a little blue light scattered back (wv, added, with the
			// ripples and the glints). No foam (the user: no white at the shores).
			float t = d / 1400.0f; if (t < 0) t = 0; if (t > 1) t = 1;
			float e = powf(t, 0.7f);
			Vtx* v = &c->wv[k];
			v->p[0] = x * 128 - 4096; v->p[1] = y * 128 - 4096; v->p[2] = 0; v->p[3] = 0;
			v->t[0] = v->t[1] = 0;
			float sh = d <= 0 ? 0 : d < 40 ? d / 40.0f : 1;   // no hard edge where the water meets the shore
			// (crystal, not blue: the user) a faint, nearly neutral shimmer
			// (natural mountain water, the user's reference: a faint olive-grey, darker green-grey deep)
			// the shore fade in the alpha: it scales the whole added layer, the sky's reflection too
			const u8 (*T)[3] = TINTS[waterTint];
			v->c[0] = T[2][0]; v->c[1] = T[2][1]; v->c[2] = T[2][2];
			v->c[3] = (u8)(255 * sh);
			if (c->tv) {
				Vtx* tv = &c->tv[k];
				*tv = *v;
				tv->c[3] = 255;
				tv->c[0] = (u8)(250 + (T[0][0] - 250) * e); tv->c[1] = (u8)(252 + (T[0][1] - 252) * e);
				tv->c[2] = (u8)(255 + (T[0][2] - 255) * e);
				tv->c[3] = 255;
			}
			// the caustics: on the bottom, strongest in the shallows (~40-150), gone by ~900
			if (c->cv) {
				float k = d <= 0 ? 0 : d < 40 ? d / 40.0f : d < 150 ? 1 : 1 - (d - 150) / 750.0f;
				if (k < 0) k = 0;
				Vtx* cvv = &c->cv[y * 65 + x];
				cvv->p[0] = v->p[0]; cvv->p[1] = v->p[1];
				cvv->p[2] = c->hgt ? (s16)lrintf(c->hgt[y * 65 + x]) : -2000; cvv->p[3] = 0;
				cvv->t[0] = cvv->t[1] = 0;
				cvv->c[0] = (u8)(70 * k); cvv->c[1] = (u8)(85 * k); cvv->c[2] = (u8)(80 * k); cvv->c[3] = 255;
			}
		}
	int n = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			int a = y * 65 + x, b = a + 1, cc = a + 65, d = cc + 1;
			if (dep[a] < -60 && dep[b] < -60 && dep[cc] < -60 && dep[d] < -60) continue;   // land
			c->wi[n++] = a; c->wi[n++] = b; c->wi[n++] = d;
			c->wi[n++] = a; c->wi[n++] = d; c->wi[n++] = cc;
		}
	c->wni = n;
	BufInfo_Init(&c->wbuf);
	BufInfo_Add(&c->wbuf, c->wv, sizeof(Vtx), 3, 0x210);
	GSPGPU_FlushDataCache(c->wv, 65 * 65 * sizeof(Vtx));
	if (c->tv) {
		BufInfo_Init(&c->tbuf);
		BufInfo_Add(&c->tbuf, c->tv, sizeof(Vtx), 3, 0x210);
		GSPGPU_FlushDataCache(c->tv, 65 * 65 * sizeof(Vtx));
	}
	if (c->cv) {
		BufInfo_Init(&c->cbuf);
		BufInfo_Add(&c->cbuf, c->cv, sizeof(Vtx), 3, 0x210);
		GSPGPU_FlushDataCache(c->cv, 65 * 65 * sizeof(Vtx));
	}
	GSPGPU_FlushDataCache(c->wi, n * sizeof(u16));
}

// The tint changed: every loaded cell's water built again (its colours are in the vertices).
// After C3D_FrameBegin only: the GPU is done with the old buffers then.
static void rebuildWater(void)
{
	for (int i = 0; i < MAX_CELLS; i++) {
		Cell* c = &cells[i];
		if (!c->used || !c->wv) continue;
		linearFree(c->wv); linearFree(c->wi);
		if (c->cv) linearFree(c->cv);
		if (c->tv) linearFree(c->tv);
		c->wv = c->cv = c->tv = NULL; c->wi = NULL; c->wni = 0;
		buildWater(c);
	}
}

// The terrain height at a world position (bilinear, like the baker's ground()). False while
// that cell is not loaded; a cell without land is the sea (-10000).
static bool groundAt(float x, float y, float* z)
{
	int gx = (int)floorf(x / CELL), gy = (int)floorf(y / CELL);
	Cell* c = findCell(gx, gy);
	if (!c) return false;
	if (!c->hgt) { *z = -10000.0f; return true; }
	float fx = (x - gx * CELL) / 128.0f, fy = (y - gy * CELL) / 128.0f;
	int ix = (int)fx, iy = (int)fy;
	if (ix > 63) ix = 63;
	if (iy > 63) iy = 63;
	if (ix < 0) ix = 0;
	if (iy < 0) iy = 0;
	float u = fx - ix, v = fy - iy;
	const float* h = c->hgt + iy * 65 + ix;
	*z = h[0] * (1 - u) * (1 - v) + h[1] * u * (1 - v) + h[65] * (1 - u) * v + h[66] * u * v;
	return true;
}

// The floors per 1024-unit square of the cell (8 x 8): which triangles lie over it, so a look-up
// tests a few, not all of them.
static void buildFloorBuckets(Cell* c)
{
	if (!c->nflo) return;
	u16 cnt[8][8] = { { 0 } };
	for (int pass = 0; pass < 2; pass++) {
		for (int t = 0; t < c->nflo && t < 65535; t++) {
			const float* v = c->flo + t * 9;
			float x0 = fminf(v[0], fminf(v[3], v[6])), x1 = fmaxf(v[0], fmaxf(v[3], v[6]));
			float y0 = fminf(v[1], fminf(v[4], v[7])), y1 = fmaxf(v[1], fmaxf(v[4], v[7]));
			int bx0 = (int)floorf((x0 + HALF) / 1024.0f), bx1 = (int)floorf((x1 + HALF) / 1024.0f);
			int by0 = (int)floorf((y0 + HALF) / 1024.0f), by1 = (int)floorf((y1 + HALF) / 1024.0f);
			if (bx0 < 0) bx0 = 0;
			if (by0 < 0) by0 = 0;
			if (bx1 > 7) bx1 = 7;
			if (by1 > 7) by1 = 7;
			for (int by = by0; by <= by1; by++)
				for (int bx = bx0; bx <= bx1; bx++) {
					if (pass == 0) cnt[by][bx]++;
					else if (c->floB[by][bx]) { u16* l = c->floB[by][bx]; l[1 + l[0]++] = (u16)t; }
				}
		}
		if (pass == 0)
			for (int by = 0; by < 8; by++)
				for (int bx = 0; bx < 8; bx++)
					if (cnt[by][bx]) c->floB[by][bx] = calloc(1 + cnt[by][bx], sizeof(u16));
	}
}

// The highest floor under (x, y) not above z + step (a bridge one walks onto, not one overhead),
// or -1e9 without one.
static float floorAt(float x, float y, float z, float step)
{
	int gx = (int)floorf(x / CELL), gy = (int)floorf(y / CELL);
	Cell* c = findCell(gx, gy);
	if (!c || !c->nflo) return -1e9f;
	float lx = x - gx * CELL - HALF, ly = y - gy * CELL - HALF;
	int bx = (int)((lx + HALF) / 1024.0f), by = (int)((ly + HALF) / 1024.0f);
	if (bx < 0 || bx > 7 || by < 0 || by > 7 || !c->floB[by][bx]) return -1e9f;
	const u16* l = c->floB[by][bx];
	float best = -1e9f;
	for (int k = 0; k < l[0]; k++) {
		const float* v = c->flo + l[1 + k] * 9;
		float e0x = v[3] - v[0], e0y = v[4] - v[1], e1x = v[6] - v[0], e1y = v[7] - v[1];
		float d = e0x * e1y - e0y * e1x;
		if (fabsf(d) < 1e-3f) continue;
		float px = lx - v[0], py = ly - v[1];
		float a = (px * e1y - py * e1x) / d, b = (e0x * py - e0y * px) / d;
		if (a < -0.001f || b < -0.001f || a + b > 1.002f) continue;
		float h = v[2] + a * (v[5] - v[2]) + b * (v[8] - v[2]);
		if (h <= z + step && h > best) best = h;
	}
	return best;
}

// Where the feet stand at (x, y) coming from height z: the terrain or a floor (stepping up at most
// `step`). False while the cell is not in.
static bool standAt(float x, float y, float z, float step, float* out)
{
	float g;
	if (!groundAt(x, y, &g)) return false;
	// the floors under the feet's circle, not just its centre: planks with gaps between them (a bridge)
	// stay one floor (the user: the character bounced from board to board)
	float f = floorAt(x, y, z, step);
	const float R = 14.0f;
	const float o[4][2] = { { R, 0 }, { -R, 0 }, { 0, R }, { 0, -R } };
	for (int i = 0; i < 4; i++) { float q = floorAt(x + o[i][0], y + o[i][1], z, step); if (q > f) f = q; }
	*out = f > g ? f : g;
	return true;
}

static void unloadCell(Cell* c)
{
	for (int i = 0; i < 64; i++) free(c->floB[i / 8][i % 8]);
	if (c->hasT) C3D_TexDelete(&c->ttex);
	free(c->bufs);
	free(c->hgt);
	if (c->wv) linearFree(c->wv);
	if (c->wi) linearFree(c->wi);
	if (c->cv) linearFree(c->cv);
	if (c->tv) linearFree(c->tv);
	linearFree(c->data);
	memset(c, 0, sizeof *c);
}

// ------------------------------------------------------------------ loader thread

enum { JOB_IDLE, JOB_READING, JOB_DONE, JOB_FAILED };
static struct {
	volatile int state;
	int gx, gy;
	u8* data;
	u32 size;
	float ioMs;
	u64 posted;
} job;
static LightEvent jobEvent;
static Thread loaderThread;
static volatile bool loaderQuit;

static void loaderMain(void* arg)
{
	(void)arg;
	while (true) {
		LightEvent_Wait(&jobEvent);
		if (loaderQuit) break;
		if (job.state != JOB_READING) continue;
		u64 t0 = ticks();
		char path[64];
		snprintf(path, sizeof path, "%scells/%d_%d.bin", dataDir, job.gx, job.gy);
		FILE* f = fopen(path, "rb");
		bool ok = f != NULL;
		// in 64 KB pieces: between them the main thread runs (this thread is above it, on its core)
		for (u32 off = 0; ok && off < job.size; off += 0x10000) {
			u32 n = job.size - off < 0x10000 ? job.size - off : 0x10000;
			if (fread(job.data + off, 1, n, f) != n) ok = false;
		}
		if (f) fclose(f);
		job.ioMs = msSince(t0);
		__sync_synchronize();
		job.state = ok ? JOB_DONE : JOB_FAILED;
	}
}

// The main thread's part of a load: the cell's buffers and terrain texture, once the file is in.
static float finishJob(void)
{
	u64 t0 = ticks();
	Cell* c = NULL;
	for (int i = 0; i < MAX_CELLS; i++) if (!cells[i].used) { c = &cells[i]; break; }
	CellHdr* h = (CellHdr*)job.data;
	if (!c || (memcmp(h->magic, "MWC1", 4) && memcmp(h->magic, "MWC2", 4) && memcmp(h->magic, "MWC3", 4))) { linearFree(job.data); return -1; }
	c->used = true; c->gx = job.gx; c->gy = job.gy; c->data = job.data; c->size = job.size; c->hdr = h;
	c->b = (BatchRec*)(job.data + h->bofs);
	c->col = NULL; c->ncol = 0;
	if (!memcmp(h->magic, "MWC2", 4) && h->ncol && h->colofs + h->ncol * 32 <= job.size) {
		c->col = (const float*)(job.data + h->colofs);
		c->ncol = h->ncol;
	}
	c->flo = NULL; c->nflo = 0; c->occ = NULL; c->occU = NULL;
	if (!memcmp(h->magic, "MWC3", 4)) {
		if (h->nfloor && h->flofs + h->nfloor * 36 <= job.size) { c->flo = (const float*)(job.data + h->flofs); c->nflo = h->nfloor; }
		if (h->occofs && h->occofs + 512 * 512 / 8 <= job.size) c->occ = job.data + h->occofs;
		c->occU = c->occ;
		if (h->ncol && h->ncol + 512 * 512 / 8 <= job.size) c->occU = job.data + h->ncol;
		buildFloorBuckets(c);
	}
	c->bufs = calloc(h->nbatch, sizeof(C3D_BufInfo));
	c->tris = 0;
	for (int i = 0; i < h->nbatch; i++) {
		BufInfo_Init(&c->bufs[i]);
		BufInfo_Add(&c->bufs[i], job.data + c->b[i].vofs, 16, 3, 0x210);
		c->tris += c->b[i].icount / 3;
		if (c->b[i].tex == 0xFFFF) {   // terrain: a 128-unit grid, x y z relative to the cell centre
			if (!c->hgt) c->hgt = calloc(65 * 65, sizeof(float));
			const s16* v = (const s16*)(job.data + c->b[i].vofs);
			for (u32 k = 0; c->hgt && k < c->b[i].vcount; k++, v += 8) {
				int ix = (v[0] + 4096) / 128, iy = (v[1] + 4096) / 128;
				if (ix >= 0 && ix <= 64 && iy >= 0 && iy <= 64) c->hgt[iy * 65 + ix] = v[2];
			}
		}
	}
	if (h->tsize) {   // the side in the low 12 bits, the format in the top 4
		int ts = h->tsize & 0xFFF, tf = h->tsize >> 12;
		c->hasT = texFrom(&c->ttex, ts, ts, tf, job.data + h->ttofs, 0xFFFFFFFFu, false);
	}
	for (int i = 0; i < h->nbatch; i++)
		if (c->b[i].flags & 4) { buildWater(c); break; }
	GSPGPU_FlushDataCache(job.data, job.size);
	return msSince(t0);
}

// A cell is wanted within `radius` rings of the camera's cell; while moving, the next ring
// ahead too (prefetch: it is loaded before the camera gets there). Nearest to where the
// camera will be in 2 s first.
static int cellsMissing;   // wanted cells within radius not loaded yet (the world is incomplete)

// The distance from a point to a cell's square (0 inside).
static float rectDist(int gx, int gy, float x, float y)
{
	float dx = fmaxf(fmaxf(gx * CELL - x, x - (gx + 1) * CELL), 0);
	float dy = fmaxf(fmaxf(gy * CELL - y, y - (gy + 1) * CELL), 0);
	return sqrtf(dx * dx + dy * dy);
}

static void pickJob(void)
{
	const float* at = rtsMode ? chr : eye;
	int cgx = (int)floorf(at[0] / CELL), cgy = (int)floorf(at[1] / CELL);
	float px = eye[0] + vel[0] * 2.0f, py = eye[1] + vel[1] * 2.0f;
	float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
	float best = 1e30f;
	const Avail* pick = NULL;
	cellsMissing = 0;
	for (int dy = -radius - 1; dy <= radius + 1; dy++)
		for (int dx = -radius - 1; dx <= radius + 1; dx++) {
			int gx = cgx + dx, gy = cgy + dy;
			bool inner;
			if (rtsMode) {   // the camera sees ~1800 round the character: no need for the whole 3 x 3
				float rd = rectDist(gx, gy, chr[0], chr[1]);
				if (rd > loadDist + 2500.0f) continue;
				inner = rd < loadDist;
			} else inner = abs(dx) <= radius && abs(dy) <= radius;
			const Avail* a = findAvail(gx, gy);
			if (!a || findCell(gx, gy)) continue;
			if (gx >= 100 && (gx != cgx || gy != cgy)) continue;   // an interior: only the one the character is in
			if (job.state != JOB_IDLE && job.gx == gx && job.gy == gy) { if (inner) cellsMissing++; continue; }
			float cx = gx * CELL + HALF - eye[0], cy = gy * CELL + HALF - eye[1];
			if (!inner) {   // the outer ring: only ahead, while moving
				if (sp < 100.0f || (cx * vel[0] + cy * vel[1]) < 0.5f * sp * sqrtf(cx * cx + cy * cy)) continue;
			} else cellsMissing++;
			float ex = gx * CELL + HALF - px, ey = gy * CELL + HALF - py;
			float d = ex * ex + ey * ey + (inner ? 0 : 4 * CELL * CELL);
			if (d < best) { best = d; pick = a; }
		}
	if (job.state != JOB_IDLE || !pick) return;
	u8* data = (u8*)linearAlloc(pick->size);
	if (!data) { loadFailed++; return; }
	job.gx = pick->gx; job.gy = pick->gy; job.data = data; job.size = pick->size;
	job.posted = ticks();
	__sync_synchronize();
	job.state = JOB_READING;
	LightEvent_Signal(&jobEvent);
}

// Unloads cells 2 rings away (hysteresis: turning back at a border keeps them), takes in a
// read cell, posts the next read. Returns the main thread's ms spent on a finished load, or 0.
static float streamCells(void)
{
	int cgx = (int)floorf(eye[0] / CELL), cgy = (int)floorf(eye[1] / CELL);
	for (int i = 0; i < MAX_CELLS; i++) {
		Cell* c = &cells[i];
		if (!c->used) continue;
		if (rtsMode ? rectDist(c->gx, c->gy, chr[0], chr[1]) > dropDist
		            : (abs(c->gx - cgx) > radius + 1 || abs(c->gy - cgy) > radius + 1)) unloadCell(c);
	}
	float setup = 0;
	if (job.state == JOB_DONE) {
		setup = finishJob();
		lastIoMs = job.ioMs;
		lastSetupMs = setup;
		loadsTotal++;
		logw("    load %d,%d: %lu KB, read %.0f ms (posted %.0f ms ago), setup %.1f ms\n", job.gx, job.gy,
		     job.size / 1024, job.ioMs, msSince(job.posted), setup);
		job.state = JOB_IDLE;
	} else if (job.state == JOB_FAILED) {
		linearFree(job.data);
		loadFailed++;
		job.state = JOB_IDLE;
	}
	pickJob();
	return setup;
}

// ------------------------------------------------------------------ drawing

static void setupTexEnv(u8 opacity, bool terrain)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvColor(env, (u32)opacity << 24 | 0xFFFFFF);
	// the terrain: x the detail texture x2 (unit 1, texcoord 1); the rest: as it is
	env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	if (terrain && detailOk) {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, 0);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	} else {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	}
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	// fog: lerp towards the sky by the vertex alpha (1 = clear)
	env = C3D_GetTexEnv(2);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, fogColour);
}

static void drawWorld(const C3D_Mtx* mvp, const float fwd[3])
{
	C3D_BindProgram(&prog);
	C3D_SetAttrInfo(&attr);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, mvp);
	float fs = farDist * fogStart, fe = farDist, k = 1.0f / (fe - fs);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, -fwd[0] * k, -fwd[1] * k, -fwd[2] * k, fe * k);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uDetail, 0, 0, 1.0f / 256.0f, 0);
	if (torchOn && !swimming && rtsMode) {   // reach ~520, warm; flickering a little
		float r = 520.0f * (0.94f + 0.06f * torchFlicker), f = torchFlicker;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTorch, torchPos[0] - eye[0], torchPos[1] - eye[1], torchPos[2] - eye[2], 1.0f / (r * r));
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTorchCol, 0.75f * f, 0.48f * f, 0.22f * f, 0);
	} else {
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTorch, 0, 0, 0, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTorchCol, 0, 0, 0, 0);
	}
	if (detailOk) C3D_TexBind(1, &detailTex);

	// frustum planes (camera-relative): left, right, bottom, top
	float pl[4][4];
	for (int i = 0; i < 4; i++) {
		int row = i >> 1;
		float sgn = (i & 1) ? -1.0f : 1.0f;
		pl[i][0] = mvp->r[3].x + sgn * mvp->r[row].x;
		pl[i][1] = mvp->r[3].y + sgn * mvp->r[row].y;
		pl[i][2] = mvp->r[3].z + sgn * mvp->r[row].z;
		pl[i][3] = mvp->r[3].w + sgn * mvp->r[row].w;
		float l = sqrtf(pl[i][0] * pl[i][0] + pl[i][1] * pl[i][1] + pl[i][2] * pl[i][2]);
		for (int j = 0; j < 4; j++) pl[i][j] /= l;
	}

	statDraws = statCulled = statSmall = statLod = 0;
	statTris = 0;
	float smallDist = farDist < 3000.0f ? farDist : 3000.0f;
	for (int pass = 0; pass < 3; pass++) {
		if (pass == 0) {
			C3D_AlphaTest(false, GPU_ALWAYS, 0);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
			C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		} else if (pass == 1) {
			C3D_AlphaTest(true, GPU_GREATER, 127);
		} else {
			C3D_AlphaTest(false, GPU_ALWAYS, 0);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
			               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		}
		int lastTex = -2, lastCull = -1, lastOpacity = -1, lastTerr = -1;
		for (int ci = 0; ci < MAX_CELLS; ci++) {
			Cell* c = &cells[ci];
			if (!c->used) continue;
			float ox = c->gx * CELL + HALF - eye[0], oy = c->gy * CELL + HALF - eye[1], oz = -eye[2];
			// the whole cell beyond the draw distance: skip it without looking at its batches
			float dx = fabsf(ox) - HALF, dy = fabsf(oy) - HALF;
			if (dx > farDist || dy > farDist) continue;
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uOrigin, ox, oy, oz, 1.0f / 1024.0f);
			for (int i = 0; i < c->hdr->nbatch; i++) {
				BatchRec* b = &c->b[i];
				int bp = (b->flags & 4) ? 2 : (b->flags & 1) ? 1 : 0;
				if (bp != pass) continue;
				float x = b->c[0] + ox, y = b->c[1] + oy, z = b->c[2] + oz;
				float d = sqrtf(x * x + y * y + z * z) - b->r;
				bool out = d > farDist;
				// small things (flag 8) only near; things wholly under the water (16) never while the camera is
				// above it (the water is opaque enough to hide them: Maniek, hardware test 2)
				if ((b->flags & 8) && d > smallDist) { statSmall++; out = true; }
				else if ((b->flags & 16) && eye[2] > 0 && (fxMode == FX_PLAIN || d > 2000.0f)) { statSmall++; out = true; }
				else if ((b->flags & 4) && fxMode != FX_PLAIN && c->wv) out = true;   // drawn by drawWater
				// LOD: big things whole up to LOD_DIST (flag 32), their lighter copy beyond it (flag 64)
				else if ((b->flags & 32) && d > LOD_DIST) out = true;
				else if ((b->flags & 64) && d <= LOD_DIST) out = true;
				if (!out && (b->flags & 64)) statLod++;
				for (int p = 0; p < 4 && !out; p++)
					if (pl[p][0] * x + pl[p][1] * y + pl[p][2] * z + pl[p][3] < -b->r) out = true;
				if (out) { statCulled++; continue; }
				int t = b->tex == 0xFFFF ? -1 : b->tex;
				if (t != lastTex) {
					if (t < 0) { if (c->hasT) C3D_TexBind(0, &c->ttex); }
					else if (t < ntex) C3D_TexBind(0, &texs[t]);
					lastTex = t < 0 ? -2 : t;   // terrain textures differ per cell
				}
				int cull = (b->flags & 3) ? 0 : 1;
				if (cull != lastCull) {
					C3D_CullFace(cull ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
					lastCull = cull;
				}
				int terr = t < 0;
				if (b->opacity != lastOpacity || terr != lastTerr) {
					setupTexEnv(b->opacity, terr);
					lastOpacity = b->opacity; lastTerr = terr;
				}
				C3D_SetBufInfo(&c->bufs[i]);
				C3D_DrawElements(GPU_TRIANGLES, b->icount, C3D_UNSIGNED_SHORT, c->data + b->iofs);
				statDraws++;
				statTris += b->icount / 3;
			}
		}
	}
}

// ------------------------------------------------------------------ dynamic objects (objects.c)

static bool uiOn, uiDirty = true;

// Interiors (as Morrowind: their own cells behind load doors). The baker puts each at its own grid cell far off
// the map (100 + k, 100), so the streaming, the collision and the objects work as outside; entering = moving the
// character there. Inside: the cell's fog colour, a short view, and the roof cut away (a clip plane over the head).
typedef struct { s32 gx, gy; u32 fog; char name[64]; } Interior;
static Interior* interiors;
static int ninterior;
static int inInterior = -1;        // the interior the character is in, -1 outside
static bool teleported;            // (the loader's velocity must not see the jump)

static void loadInteriors(void)
{
	u32 size;
	u8* b = readFile(dataPath("interiors.bin"), &size);
	if (!b || size < 8 || memcmp(b, "MWN1", 4)) { free(b); return; }
	memcpy(&ninterior, b + 4, 4);
	if (8 + ninterior * sizeof(Interior) > size) ninterior = 0;
	interiors = (Interior*)malloc(ninterior * sizeof(Interior) + 1);
	memcpy(interiors, b + 8, ninterior * sizeof(Interior));
	for (int i = 0; i < ninterior; i++) interiors[i].name[63] = 0;
	free(b);
}

static int interiorAt(float x, float y)
{
	int gx = (int)floorf(x / CELL), gy = (int)floorf(y / CELL);
	for (int i = 0; i < ninterior; i++) if (interiors[i].gx == gx && interiors[i].gy == gy) return i;
	return -1;
}

// The user clip plane of the PICA200 (GPUREG_FRAGOP_CLIP): what is above world height z is not drawn.
// The plane is given in clip space: a world plane a (camera-relative) becomes inverse(mvp)^T a.
static void clipAbove(const C3D_Mtx* mvp, float z)
{
	C3D_Mtx inv = *mvp;
	if (Mtx_Inverse(&inv) == 0.0f) return;
	float a[4] = { 0, 0, -1, z - eye[2] };   // keep -z + (z0 - eye z) >= 0
	float c[4];
	for (int j = 0; j < 4; j++) {
		float s = 0;
		for (int i = 0; i < 4; i++) {
			const C3D_FVec* r = &inv.r[i];
			float m = j == 0 ? r->x : j == 1 ? r->y : j == 2 ? r->z : r->w;
			s += m * a[i];
		}
		c[j] = s;
	}
	u32 d[4] = { f32tof24(c[0]), f32tof24(c[1]), f32tof24(c[2]), f32tof24(c[3]) };
	GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP, 1);
	GPUCMD_AddIncrementalWrites(GPUREG_FRAGOP_CLIP_DATA0, d, 4);
}
static void clipOff(void) { GPUCMD_AddWrite(GPUREG_FRAGOP_CLIP, 0); }

// Items, doors, containers: each copy drawn with its own matrix (obj.v.pica), only in loaded cells, not
// taken. Small things (radius < 40) only up to 2500 units. The shader's state as drawWorld left it.
static DVLB_s* oDvlb;
static shaderProgram_s oProg;
static int oMvp, oModel, oFog, oTorch, oTorchCol;
static bool objsOk, scriptsOk;
static C3D_BufInfo* objBufs;     // per model part
static int objTarget = -1;       // the object in reach (A uses it), -1 none
static int statObjDraws;
static char noteText[96];        // a script's / the game's message on the bottom screen
static float noteTime;

void gameMessage(const char* text)
{
	snprintf(noteText, sizeof noteText, "%s", text);
	noteTime = 4.0f;
	uiDirty = true;
	logw("    msg: %s\n", text);
}
void gameLogLine(const char* text) { logw("    %s\n", text); }
void gameFeet(float out[3]) { out[0] = chr[0]; out[1] = chr[1]; out[2] = chr[2]; }

static void objInitGpu(void)
{
	oDvlb = DVLB_ParseFile((u32*)obj_shbin, obj_shbin_size);
	shaderProgramInit(&oProg);
	shaderProgramSetVsh(&oProg, &oDvlb->DVLE[0]);
	oMvp = shaderInstanceGetUniformLocation(oProg.vertexShader, "mvp");
	oModel = shaderInstanceGetUniformLocation(oProg.vertexShader, "model");
	oFog = shaderInstanceGetUniformLocation(oProg.vertexShader, "fogRow");
	oTorch = shaderInstanceGetUniformLocation(oProg.vertexShader, "torch");
	oTorchCol = shaderInstanceGetUniformLocation(oProg.vertexShader, "torchCol");
	objBufs = calloc(objNParts ? objNParts : 1, sizeof(C3D_BufInfo));
	for (int i = 0; i < objNParts; i++) {
		BufInfo_Init(&objBufs[i]);
		BufInfo_Add(&objBufs[i], objData + objParts[i].vofs, 16, 3, 0x210);
	}
}

static void drawObjects(const C3D_Mtx* mvp, const float fwd[3])
{
	statObjDraws = 0;
	if (!objsOk || !nobj) return;
	C3D_BindProgram(&oProg);
	C3D_SetAttrInfo(&attr);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, oMvp, mvp);
	float fs = farDist * fogStart, fe = farDist, k = 1.0f / (fe - fs);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, oFog, -fwd[0] * k, -fwd[1] * k, -fwd[2] * k, fe * k);
	if (torchOn && !swimming && rtsMode) {
		float r = 520.0f * (0.94f + 0.06f * torchFlicker), f = torchFlicker;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oTorch, torchPos[0] - eye[0], torchPos[1] - eye[1], torchPos[2] - eye[2], 1.0f / (r * r));
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oTorchCol, 0.75f * f, 0.48f * f, 0.22f * f, 0);
	} else {
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oTorch, 0, 0, 0, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oTorchCol, 0, 0, 0, 0);
	}
	float pl[4][4];
	for (int i = 0; i < 4; i++) {
		int row = i >> 1;
		float sgn = (i & 1) ? -1.0f : 1.0f;
		pl[i][0] = mvp->r[3].x + sgn * mvp->r[row].x;
		pl[i][1] = mvp->r[3].y + sgn * mvp->r[row].y;
		pl[i][2] = mvp->r[3].z + sgn * mvp->r[row].z;
		pl[i][3] = mvp->r[3].w + sgn * mvp->r[row].w;
		float l = sqrtf(pl[i][0] * pl[i][0] + pl[i][1] * pl[i][1] + pl[i][2] * pl[i][2]);
		for (int j = 0; j < 4; j++) pl[i][j] /= l;
	}
	setupTexEnv(255, false);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	int lastTex = -2, lastCull = -1, lastAlpha = -1;
	for (int i = 0; i < nobj; i++) {
		WObj* o = &objs[i];
		if (o->flags & OF_TAKEN) continue;
		const ObjModel* m = objModel(i);
		if (!m || !m->nparts) continue;
		float x = o->pos[0] - eye[0], y = o->pos[1] - eye[1], z = o->pos[2] - eye[2];
		float r = m->radius * o->scale;
		float d = sqrtf(x * x + y * y + z * z) - r;
		if (d > farDist || (r < 40.0f && d > 2500.0f)) continue;
		bool out = false;
		for (int p = 0; p < 4 && !out; p++)
			if (pl[p][0] * x + pl[p][1] * y + pl[p][2] * z + pl[p][3] < -r) out = true;
		if (out || !findCell(o->gx, o->gy)) continue;
		float M[12];
		objMatrix(i, eye, M);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oModel, M[0], M[1], M[2], M[3]);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oModel + 1, M[4], M[5], M[6], M[7]);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, oModel + 2, M[8], M[9], M[10], M[11]);
		for (int k2 = 0; k2 < m->nparts; k2++) {
			int pi = m->first + k2;
			if (pi >= objNParts) break;
			const ObjPart* p = &objParts[pi];
			int t = p->tex < ntex ? p->tex : 0;
			if (t != lastTex) { C3D_TexBind(0, &texs[t]); lastTex = t; }
			int alpha = p->flags & 1;
			if (alpha != lastAlpha) { C3D_AlphaTest(alpha, GPU_GREATER, 127); lastAlpha = alpha; }
			int cull = (p->flags & 2) ? 0 : 1;
			if (cull != lastCull) { C3D_CullFace(cull ? GPU_CULL_BACK_CCW : GPU_CULL_NONE); lastCull = cull; }
			C3D_SetBufInfo(&objBufs[pi]);
			C3D_DrawElements(GPU_TRIANGLES, p->icount, C3D_UNSIGNED_SHORT, objData + p->iofs);
			statObjDraws++;
			statTris += p->icount / 3;
		}
	}
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_BACK_CCW);
}

// A on the object in reach: the scripts first (OnActivate may stop it), then what the object does by
// itself (an item is taken, a door / container opens or closes), then the scripts again (OnTake / OnOpen...).
static void objActivate(int i)
{
	if (i < 0 || i >= nobj) return;
	int type = objType(i);
	if (!scriptActivate(i)) { uiDirty = true; return; }
	WObj* o = &objs[i];
	if (type == OT_ITEM) {
		o->flags |= OF_TAKEN | OF_CHANGED;
		audioPlay(SND_CLICK, 0.7f, 0);
		char b[96];
		snprintf(b, sizeof b, "Zabrano: %s", objName(i));
		gameMessage(b);
		scriptAfter(i, SE_TAKE);
	} else if (type == OT_DOOR && objDest(i)) {
		// a load door: it takes the character to its destination when that cell is baked (an interior, or
		// back outside), else it only says where it leads
		const float* d = o->dest;
		char b[96];
		// (outside: "" ; an interior only when baked: an unbaked one's mark is in its own coordinates, which may fall on a baked cell outside: Census Office -> cell -1,-1 on the console)
		int dgx = (int)floorf(d[0] / CELL), dgy = (int)floorf(d[1] / CELL);
		if (findAvail(dgx, dgy) && (objDest(i)[0] == 0 ? dgx < 100 : dgx >= 100)) {
			chr[0] = d[0]; chr[1] = d[1]; chr[2] = d[2];
			chrYaw = rtsYaw = d[3];
			chrPlaced = false;          // stood on the floor once the cell is in: the one at the door's mark,
			chrPlaceUp = 40.0f;         // not a beam over it (the start outside looks from 400 up)
			chrVz = 0; swimming = false;
			teleported = true;
			objTarget = -1;
			snprintf(b, sizeof b, "%s", objDest(i)[0] ? objDest(i) : "Na zewnątrz");
		} else snprintf(b, sizeof b, "Drzwi do: %s (tego wnętrza nie ma)", objDest(i)[0] ? objDest(i) : "na zewnątrz");
		gameMessage(b);
		audioPlay(SND_CLICK, 0.5f, 0);
	} else if (o->flags & OF_LOCKED) {
		char b[96];
		snprintf(b, sizeof b, "%s: zamknięte na klucz", objName(i));
		gameMessage(b);
	} else {
		bool open = !(o->flags & OF_OPEN);
		o->flags = (open ? o->flags | OF_OPEN : o->flags & ~OF_OPEN) | OF_CHANGED;
		audioPlay(SND_CLICK, 0.6f, 0);
		if (type == OT_CONT) {
			char b[96];
			snprintf(b, sizeof b, "%s: %s", objName(i), open ? "otwarte (pusto)" : "zamknięte");
			gameMessage(b);
		}
		scriptAfter(i, open ? SE_OPEN : SE_CLOSE);
	}
	logw("    use #%d %s (%s, frmr %lu, cell %d,%d) -> flags %x\n", i, objId(i), objName(i), objs[i].frmr, objs[i].gx, objs[i].gy, objs[i].flags);
}

// ------------------------------------------------------------------ character (a stand-in)

// A figure of boxes until the skinned characters come (skeleton + swappable parts): legs and
// arms swing with the walk cycle. Rebuilt on the CPU each frame into one buffer (~250 vertices),
// positions relative to the feet, drawn with the world shader (origin = feet - eye).
#define CHR_MAXV 256
#define CHR_MAXI 512
static Vtx* chrV;
static u16* chrI;
static int chrNV, chrNI;
static C3D_BufInfo chrBuf;

// A box with half sizes h, hanging from a pivot (its top centre) at piv, swung by `swing` round
// the figure's sideways axis; the whole figure turned by chrYaw. Shaded per face.
static void chrBox(const float piv[3], const float h[3], float swing, u32 rgb)
{
	if (chrNV + 24 > CHR_MAXV || chrNI + 36 > CHR_MAXI) return;
	static const float shade[6] = { 0.85f, 0.6f, 0.75f, 0.7f, 1.0f, 0.45f };   // +x -x +y -y +z -z
	float cs = cosf(swing), sn = sinf(swing), cy = cosf(chrYaw), sy = sinf(chrYaw);
	for (int f = 0; f < 6; f++) {
		int ax = f >> 1, u = (ax + 1) % 3, v = (ax + 2) % 3;
		float sg = (f & 1) ? -1.0f : 1.0f;
		int base = chrNV;
		for (int k = 0; k < 4; k++) {
			float q[3];
			q[ax] = sg * h[ax];
			q[u] = (k == 1 || k == 2) ? h[u] : -h[u];
			q[v] = (k >= 2) ? h[v] : -h[v];
			q[2] -= h[2];   // the pivot is at the top
			// swing round x (sideways): y forward, z up
			float y = q[1] * cs - q[2] * sn, z = q[1] * sn + q[2] * cs;
			float x = q[0] + piv[0];
			y += piv[1]; z += piv[2];
			// turned by the facing (yaw 0 = +y, like the camera)
			float wx = x * cy + y * sy, wy = -x * sy + y * cy;
			Vtx* o = &chrV[chrNV++];
			o->p[0] = (s16)lrintf(wx); o->p[1] = (s16)lrintf(wy); o->p[2] = (s16)lrintf(z); o->p[3] = 0;
			o->t[0] = o->t[1] = 0;
			float l = shade[f];
			o->c[0] = (u8)(((rgb >> 16) & 0xFF) * l); o->c[1] = (u8)(((rgb >> 8) & 0xFF) * l);
			o->c[2] = (u8)((rgb & 0xFF) * l); o->c[3] = 255;
		}
		static const u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
		for (int k = 0; k < 6; k++) chrI[chrNI++] = base + idx[k];
	}
}

static void chrBuild(void)
{
	chrNV = chrNI = 0;
	float a = sinf(chrPhase) * 0.65f * chrSwing;   // the legs' swing
	float bob = fabsf(cosf(chrPhase)) * 3.0f * chrSwing;
	static const float leg[3] = { 7, 7, 31 }, arm[3] = { 5, 5, 26 }, body[3] = { 17, 10, 24 },
	                   head[3] = { 9, 9, 11 }, hair[3] = { 10, 10, 4 }, foot[3] = { 7, 10, 3 };
	float hip = 62 + bob, sh = 106 + bob;
	float p[3];
	p[0] = -9; p[1] = 0; p[2] = hip; chrBox(p, leg, a, 0x5A4632);
	p[0] = 9;  chrBox(p, leg, -a, 0x5A4632);
	// feet: at the ends of the swung legs, a little forward
	p[0] = -9; p[1] = 62 * sinf(a) + 4;  p[2] = hip - 62 * cosf(a) + 6;  chrBox(p, foot, 0, 0x2E2620);
	p[0] = 9;  p[1] = 62 * sinf(-a) + 4; p[2] = hip - 62 * cosf(-a) + 6; chrBox(p, foot, 0, 0x2E2620);
	p[0] = 0;  p[1] = 0; p[2] = sh + 2; chrBox(p, body, 0, 0x8C2A22);   // tunic
	p[2] = sh + 25; chrBox(p, head, 0, 0xD2A07A);
	p[2] = sh + 27; chrBox(p, hair, 0, 0x3A2A1C);
	p[0] = -23; p[2] = sh; chrBox(p, arm, -a * 0.8f, 0xC08E6A);
	p[0] = 23;  chrBox(p, arm, a * 0.8f, 0xC08E6A);
	GSPGPU_FlushDataCache(chrV, chrNV * sizeof(Vtx));
	GSPGPU_FlushDataCache(chrI, chrNI * sizeof(u16));
}

static void drawBoxes(void)
{
	if (!chrV || !chrI || !chrPlaced) return;
	chrBuild();
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uOrigin, chr[0] - eye[0], chr[1] - eye[1], chr[2] - eye[2], 1.0f / 1024.0f);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);
	setupTexEnv(255, false);               // stage 2: the fog
	C3D_TexEnv* env = C3D_GetTexEnv(0);    // stage 0: the vertex colour alone
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_SetBufInfo(&chrBuf);
	C3D_DrawElements(GPU_TRIANGLES, chrNI, C3D_UNSIGNED_SHORT, chrI);
	statDraws++;
	statTris += chrNI / 3;
}


// ------------------------------------------------------------------ character (Morrowind's, romfs:/char.bin)

// tools/chrbake.py: the skeleton's animations as the bones' matrices per frame, the body, the
// clothes / armour and the weapons as meshes in the NIF's own space, each with a palette of the
// bones it hangs on (<= 12) and per vertex up to 4 of them with weights. Skinned on the GPU
// (skin.v.pica): per draw the palette is set as facing x pose x the mesh-to-bone matrix, so a
// character costs the CPU a pose (two frames blended) and a few uniforms per mesh, not its
// vertices: that is what lets many NPCs stand round (L+A spawns them).
typedef struct { u8 bone, pad[3]; float m[12]; } CPal;
typedef struct {
	u16 tex; u8 alpha, npal;
	u32 nv, nidx;
	float col[3];
	const CPal* pal;
	void* vb; u16* idx;   // linear memory, uploaded once
	C3D_BufInfo buf;
} CMesh;
typedef struct { char name[16]; u32 frames; float fps, speed; u32 loop; const float* m; } CAnim;
typedef struct { char name[24]; u8 helmet, n; const u8* parts; } CItem;   // parts: n x { u8 slot, u8 pad, s16 group }
typedef struct { char name[16]; u8 n; const u8* items; } COutfit;
typedef struct { char name[24]; s16 group; } CWeapon;

static u8* chData;
static bool chOk;
static int chBones, chNAnim, chNMesh, chNGroup, chNItem, chNOutfit, chNWeapon;
static CAnim chAnim[24];
static CMesh* chMesh;
static const u16** chGroup;     // per group: u16 n, then n mesh ids
static s16 chBody[27];
static CItem* chItem;
static COutfit* chOutfit;
static CWeapon chWeapon[8];
static int chTorchGroup = -1, chTorchBone, chNeck;
static float chFlame[3];        // the flame in the torch bone's space
static u8 chArm[64];            // the left arm's bones (the torch pose laid on them)
static const float* chTorchPose;
static C3D_AttrInfo skinAttr;
static shaderProgram_s sProg;
static DVLB_s* sDvlb;
static int sMvp, sOrigin, sFog, sSun, sMcol, sTorch, sTorchCol, sBones;
// the animations by name (tools/chrbake.py ANIMS), -1 missing
enum { A_IDLE, A_WALK, A_RUN, A_SWIMIDLE, A_SWIMWALK, A_SWIMRUN, A_JUMP, A_IDLE1H, A_WALK1H, A_RUN1H,
       A_SLASH1H, A_THRUST1H, A_CHOP1H, A_SLASHHH, A_THRUSTHH, A_CHOPHH, A_HIT, A_DEATH, NA };
static int chA[NA];

// One character on the screen: the player (actors[0]) or an NPC.
#define MAX_ACTORS 64
typedef struct {
	bool used;
	float pos[3], yaw, pitch;      // pitch: the body tipped forward (diving: head down)
	int outfit, weapon;            // weapon: -1 none; outfit: NPCs (the player wears `worn`)
	bool custom;                   // wears worn[] (single pieces) instead of an outfit
	u8 worn[64];
	bool helmet, torch;
	int cur, prev;                 // animations
	float t, prevT, fade;
	float speed;                   // units/s it moves (the walk's pace)
	bool run, swim, air;
	int act;                       // a one-off playing (A_SLASH1H.. A_HIT, A_DEATH), -1 none
	bool dead;                     // (the death held on its last frame)
	int hp, maxHp;
	float pose[64][12];
	int shown[96], nshown, verts;
	bool dirty;                    // what it wears changed
} Actor;
static Actor actors[MAX_ACTORS];
static int nNpc;
#define PL (&actors[0])
static float chAnimMs, chDrawMs;   // last frame: the poses, the drawing (CPU)

static bool loadCharacter(void)
{
	u32 size;
	chData = readFile(dataPath("char.bin"), &size);
	if (!chData || memcmp(chData, "MWCK", 4)) { free(chData); chData = NULL; return false; }
	const u8* p = chData + 4;
	u32 h[7];
	memcpy(h, p, 28); p += 28;
	chBones = h[0]; chNAnim = h[1]; chNMesh = h[2]; chNGroup = h[3]; chNItem = h[4]; chNOutfit = h[5]; chNWeapon = h[6];
	if (chBones > 64 || chNAnim > 24 || chNWeapon > 8) return false;
	for (int i = 0; i < chNAnim; i++) {
		memcpy(chAnim[i].name, p, 16); p += 16;
		memcpy(&chAnim[i].frames, p, 4); memcpy(&chAnim[i].fps, p + 4, 4); memcpy(&chAnim[i].speed, p + 8, 4);
		memcpy(&chAnim[i].loop, p + 12, 4); p += 16;
		chAnim[i].m = (const float*)p;
		p += chAnim[i].frames * chBones * 12 * 4;
	}
	chMesh = calloc(chNMesh, sizeof(CMesh));
	for (int i = 0; i < chNMesh; i++) {
		CMesh* m = &chMesh[i];
		memcpy(&m->tex, p, 2); m->alpha = p[2]; m->npal = p[3];
		memcpy(&m->nv, p + 4, 4); memcpy(&m->nidx, p + 8, 4); memcpy(m->col, p + 12, 12);
		p += 24;
		m->pal = (const CPal*)p; p += m->npal * sizeof(CPal);
		m->vb = linearAlloc(m->nv * 32);
		m->idx = (u16*)linearAlloc(m->nidx * 2 + 4);
		if (!m->vb || !m->idx) return false;
		memcpy(m->vb, p, m->nv * 32); p += m->nv * 32;
		memcpy(m->idx, p, m->nidx * 2); p += (m->nidx * 2 + 3) & ~3u;
		GSPGPU_FlushDataCache(m->vb, m->nv * 32);
		GSPGPU_FlushDataCache(m->idx, m->nidx * 2);
		BufInfo_Init(&m->buf);
		BufInfo_Add(&m->buf, m->vb, 32, 5, 0x43210);
	}
	chGroup = calloc(chNGroup, sizeof(u16*));
	for (int i = 0; i < chNGroup; i++) {
		chGroup[i] = (const u16*)p;
		u16 n = chGroup[i][0];
		p += (1 + n) * 2;
		if ((n + 1) % 2) p += 2;
	}
	memcpy(chBody, p, 54); p += 56;
	chItem = calloc(chNItem, sizeof(CItem));
	for (int i = 0; i < chNItem; i++) {
		memcpy(chItem[i].name, p, 24); chItem[i].name[23] = 0;
		chItem[i].helmet = p[24]; chItem[i].n = p[25]; p += 26;
		chItem[i].parts = p; p += chItem[i].n * 4 + 2;
	}
	chOutfit = calloc(chNOutfit, sizeof(COutfit));
	for (int i = 0; i < chNOutfit; i++) {
		memcpy(chOutfit[i].name, p, 16); chOutfit[i].name[15] = 0;
		chOutfit[i].n = p[16]; chOutfit[i].items = p + 17;
		p += (17 + chOutfit[i].n + 3) & ~3u;
	}
	for (int i = 0; i < chNWeapon; i++) {
		memcpy(chWeapon[i].name, p, 24); chWeapon[i].name[23] = 0;
		memcpy(&chWeapon[i].group, p + 24, 2);
		p += 28;
	}
	if (p + 20 <= chData + size && !memcmp(p, "TRCH", 4)) {
		s16 g; memcpy(&g, p + 4, 2);
		chTorchGroup = g; chTorchBone = p[6]; chNeck = p[7];
		memcpy(chFlame, p + 8, 12);
		p += 20;
		memcpy(chArm, p, chBones); p += (chBones + 3) & ~3u;
		chTorchPose = (const float*)p;
		if (chTorchBone >= chBones || chNeck >= chBones) chTorchGroup = -1;
	}
	static const char* const names[NA] = { "idle", "walk", "run", "swimidle", "swimwalk", "swimrun", "jump",
	                                       "idle1h", "walk1h", "run1h",
	                                       "slash1h", "thrust1h", "chop1h", "slashhh", "thrusthh", "chophh", "hit", "death" };
	for (int k = 0; k < NA; k++) {
		chA[k] = -1;
		for (int i = 0; i < chNAnim; i++) if (!strcmp(chAnim[i].name, names[k])) chA[k] = i;
	}
	if (chA[A_IDLE] < 0) chA[A_IDLE] = 0;
	// the skinning shader
	sDvlb = DVLB_ParseFile((u32*)skin_shbin, skin_shbin_size);
	shaderProgramInit(&sProg);
	shaderProgramSetVsh(&sProg, &sDvlb->DVLE[0]);
	sMvp = shaderInstanceGetUniformLocation(sProg.vertexShader, "mvp");
	sOrigin = shaderInstanceGetUniformLocation(sProg.vertexShader, "origin");
	sFog = shaderInstanceGetUniformLocation(sProg.vertexShader, "fogRow");
	sSun = shaderInstanceGetUniformLocation(sProg.vertexShader, "sun");
	sMcol = shaderInstanceGetUniformLocation(sProg.vertexShader, "mcol");
	sTorch = shaderInstanceGetUniformLocation(sProg.vertexShader, "torch");
	sTorchCol = shaderInstanceGetUniformLocation(sProg.vertexShader, "torchCol");
	sBones = shaderInstanceGetUniformLocation(sProg.vertexShader, "bones");
	AttrInfo_Init(&skinAttr);
	AttrInfo_AddLoader(&skinAttr, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(&skinAttr, 1, GPU_SHORT, 4);
	AttrInfo_AddLoader(&skinAttr, 2, GPU_SHORT, 2);
	AttrInfo_AddLoader(&skinAttr, 3, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(&skinAttr, 4, GPU_UNSIGNED_BYTE, 4);
	memset(PL, 0, sizeof *PL);
	PL->used = true; PL->outfit = chNOutfit > 1 ? 1 : 0; PL->helmet = true; PL->weapon = -1;
	PL->custom = true;   // starts in the commoner's clothes, piece by piece
	for (int k = 0; k < chOutfit[PL->outfit].n; k++) if (chOutfit[PL->outfit].items[k] < 64) PL->worn[chOutfit[PL->outfit].items[k]] = 1;
	PL->prev = -1; PL->dirty = true;
	PL->act = -1; PL->hp = PL->maxHp = 100;
	chOk = true;
	return true;
}

// What the slots show: the body, unless the outfit's items cover them (a helmet off: its slots
// left as they are); any helmet hides the hair; the weapon and the torch in the hands.
static void chCompose(Actor* a)
{
	s16 slot[27];
	memcpy(slot, chBody, sizeof slot);
	bool hideHair = false;
	const COutfit* o = &chOutfit[a->outfit];
	int n = a->custom ? (chNItem < 64 ? chNItem : 64) : o->n;
	for (int k = 0; k < n; k++) {
		if (a->custom && !a->worn[k]) continue;
		const CItem* it = &chItem[a->custom ? k : o->items[k]];
		if (!a->custom && it->helmet && !a->helmet) continue;
		if (it->helmet) hideHair = true;
		for (int j = 0; j < it->n; j++) {
			u8 sl = it->parts[j * 4];
			s16 g; memcpy(&g, it->parts + j * 4 + 2, 2);
			if (sl < 27) slot[sl] = g;
		}
	}
	if (hideHair) slot[1] = -1;
	if (a->torch && !a->swim && chTorchGroup >= 0 && chTorchGroup < chNGroup) slot[10] = chTorchGroup;
	if (a->weapon >= 0 && a->weapon < chNWeapon) slot[25] = chWeapon[a->weapon].group;
	a->nshown = 0;
	a->verts = 0;
	for (int sl = 0; sl < 27; sl++) {
		int g = slot[sl];
		if (g < 0 || g >= chNGroup) continue;
		bool dup = false;
		for (int k = 0; k < sl; k++) if (slot[k] == g) { dup = true; break; }
		if (dup) continue;
		const u16* gr = chGroup[g];
		for (int k = 0; k < gr[0] && a->nshown < 96; k++) {
			a->shown[a->nshown++] = gr[1 + k];
			a->verts += chMesh[gr[1 + k]].nv;
		}
	}
	a->dirty = false;
}

// A frame of an animation: looped, or (a one-off) held on its last frame.
static void chSample(int an_, float t, float out[][12])
{
	const CAnim* an = &chAnim[an_];
	float f = t * an->fps;
	int n = an->frames, i, j;
	float k;
	if (an->loop) {
		f = fmodf(f, (float)n);
		if (f < 0) f += n;
		i = (int)f; j = (i + 1) % n; k = f - i;
	} else {
		if (f > n - 1) f = n - 1;
		if (f < 0) f = 0;
		i = (int)f; j = i + 1 < n ? i + 1 : i; k = f - i;
	}
	const float* A = an->m + (size_t)i * chBones * 12;
	const float* B = an->m + (size_t)j * chBones * 12;
	for (int b = 0; b < chBones * 12; b++) out[0][b] = A[b] + (B[b] - A[b]) * k;
}

static float animLength(int an) { return chAnim[an].frames / chAnim[an].fps; }

// 3x4 affine matrices (rows): out = a x b
static void aff(float* out, const float* a, const float* b)
{
	for (int r = 0; r < 3; r++) {
		const float* x = a + r * 4;
		out[r * 4 + 0] = x[0] * b[0] + x[1] * b[4] + x[2] * b[8];
		out[r * 4 + 1] = x[0] * b[1] + x[1] * b[5] + x[2] * b[9];
		out[r * 4 + 2] = x[0] * b[2] + x[1] * b[6] + x[2] * b[10];
		out[r * 4 + 3] = x[0] * b[3] + x[1] * b[7] + x[2] * b[11] + x[3];
	}
}

static void affInverse(float* out, const float* m)
{
	float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
	float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	float det = a * A + b * B + c * C;
	if (fabsf(det) < 1e-9f) det = 1e-9f;
	float id = 1.0f / det;
	float r[9] = { A * id, -(b * i - c * h) * id, (b * f - c * e) * id,
	               B * id, (a * i - c * g) * id, -(a * f - c * d) * id,
	               C * id, -(a * h - b * g) * id, (a * e - b * d) * id };
	for (int k = 0; k < 3; k++) {
		out[k * 4 + 0] = r[k * 3]; out[k * 4 + 1] = r[k * 3 + 1]; out[k * 4 + 2] = r[k * 3 + 2];
		out[k * 4 + 3] = -(r[k * 3] * m[3] + r[k * 3 + 1] * m[7] + r[k * 3 + 2] * m[11]);
	}
}

// The animation by what the actor does: attacking, in the air, swimming, else idle / walk / run
// (the one-handed set with a weapon); a change fades over 0.2 s. With the torch the left arm
// takes the "Torch" pose, carried along with the neck.
static void chAnimate(Actor* a, float dt)
{
	bool moving = a->speed > 20.0f;
	bool armed = a->weapon >= 0 && chA[A_IDLE1H] >= 0;
	int k;
	if (a->dead) k = A_DEATH;
	else if (a->act >= 0) k = a->act;
	else if (a->air && !a->swim) k = A_JUMP;
	else if (a->swim) k = moving ? (a->run ? A_SWIMRUN : A_SWIMWALK) : A_SWIMIDLE;
	else if (armed) k = moving ? (a->run ? A_RUN1H : A_WALK1H) : A_IDLE1H;
	else k = moving ? (a->run ? A_RUN : A_WALK) : A_IDLE;
	int want = chA[k];
	if (want < 0) want = moving && chA[A_WALK] >= 0 ? chA[A_WALK] : chA[A_IDLE];
	if (want != a->cur) {
		a->prev = a->cur; a->prevT = a->t; a->fade = 1.0f;
		a->cur = want; a->t = 0;
	}
	float rate = 1.0f;
	if (chAnim[a->cur].speed > 1.0f) {
		rate = a->speed / chAnim[a->cur].speed;
		if (rate < 0.3f) rate = 0.3f;
		if (rate > 2.5f) rate = 2.5f;
	}
	if (a->act >= 0 || a->dead) rate = 1.0f;
	a->t += dt * rate;
	if (a->act >= 0 && !a->dead && a->t >= animLength(a->cur)) a->act = -1;   // done: back to the rest
	chSample(a->cur, a->t, a->pose);
	if (a->fade > 0 && a->prev >= 0) {
		static float prevPose[64][12];
		a->prevT += dt;
		chSample(a->prev, a->prevT, prevPose);
		for (int b = 0; b < chBones * 12; b++) a->pose[0][b] += (prevPose[0][b] - a->pose[0][b]) * a->fade;
		a->fade -= dt * 5.0f;
	}
	if (a->torch && !a->swim && chTorchGroup >= 0 && chTorchPose) {
		// arm = neck now x neck in the torch pose^-1 x arm in the torch pose
		float inv[12], rel[12];
		affInverse(inv, chTorchPose + chNeck * 12);
		aff(rel, a->pose[chNeck], inv);
		for (int b = 0; b < chBones; b++)
			if (chArm[b]) aff(a->pose[b], rel, chTorchPose + b * 12);
	}
}

// The actor's facing (yaw round z; the body tipped by pitch round its hips) as a 3x4 matrix.
static void chFacing(const Actor* a, float F[12])
{
	float cy = cosf(a->yaw), sy = sinf(a->yaw), cp = cosf(a->pitch), sp = sinf(a->pitch);
	const float PZ = 80.0f;   // the pivot's height
	// tip: y' = y cp - (z - PZ) sp, z' = y sp + (z - PZ) cp + PZ (pitch < 0: the head forward / down)
	float T[12] = { 1, 0, 0, 0,
	                0, cp, -sp, PZ * sp,
	                0, sp, cp, PZ - PZ * cp };
	// turn: x' = x cy + y sy, y' = -x sy + y cy (yaw 0 = +y, like the camera)
	float R[12] = { cy, sy, 0, 0, -sy, cy, 0, 0, 0, 0, 1, 0 };
	aff(F, R, T);
}

// The flame of the actor's torch in the world.
static void chFlamePos(const Actor* a, float out[3])
{
	float F[12], M[12];
	chFacing(a, F);
	aff(M, F, a->pose[chTorchBone]);
	for (int r = 0; r < 3; r++)
		out[r] = a->pos[r] + M[r * 4] * chFlame[0] + M[r * 4 + 1] * chFlame[1] + M[r * 4 + 2] * chFlame[2] + M[r * 4 + 3];
}

// The skinning program with the frame's uniforms (once per eye, before the actors).
static void skinProgram(const C3D_Mtx* mvp, const float fwd[3])
{
	C3D_BindProgram(&sProg);
	C3D_SetAttrInfo(&skinAttr);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sMvp, mvp);
	float fs = farDist * fogStart, fe = farDist, k = 1.0f / (fe - fs);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, sFog, -fwd[0] * k, -fwd[1] * k, -fwd[2] * k, fe * k);
	const float SX = -0.35f, SY = 0.45f, SZ = 0.82f, sl = 1.0f / sqrtf(SX * SX + SY * SY + SZ * SZ);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, sSun, SX * sl, SY * sl, SZ * sl, 0);
	if (torchOn && !swimming && rtsMode) {
		float r = 520.0f, f = torchFlicker * 0.45f;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, sTorch, torchPos[0] - eye[0], torchPos[1] - eye[1], torchPos[2] - eye[2], 1.0f / (r * r));
		C3D_FVUnifSet(GPU_VERTEX_SHADER, sTorchCol, 0.75f * f, 0.48f * f, 0.22f * f, 0);
	} else {
		C3D_FVUnifSet(GPU_VERTEX_SHADER, sTorch, 0, 0, 0, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, sTorchCol, 0, 0, 0, 0);
	}
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);
	setupTexEnv(255, false);
}

// One actor: culled by its sphere against the frustum planes, then per mesh the palette and a draw.
static void drawActor(Actor* a, float pl[4][4])
{
	float ox = a->pos[0] - eye[0], oy = a->pos[1] - eye[1], oz = a->pos[2] - eye[2];
	float cx = ox, cy = oy, cz = oz + 70.0f;
	if (sqrtf(cx * cx + cy * cy + cz * cz) > farDist + 100.0f) return;
	for (int p = 0; p < 4; p++)
		if (pl[p][0] * cx + pl[p][1] * cy + pl[p][2] * cz + pl[p][3] < -110.0f) return;
	if (a->dirty) chCompose(a);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, sOrigin, ox, oy, oz, 0);
	float F[12], FP[64][12];
	chFacing(a, F);
	bool done[64] = { false };
	int lastTex = -1, lastAlpha = -1;
	for (int s = 0; s < a->nshown; s++) {
		CMesh* m = &chMesh[a->shown[s]];
		int np = m->npal < 12 ? m->npal : 12;
		C3D_FVec* U = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, sBones, np * 3);   // the palette in one go
		for (int k = 0; k < np; k++) {
			int b = m->pal[k].bone;
			if (b >= chBones) b = 0;
			if (!done[b]) { aff(FP[b], F, a->pose[b]); done[b] = true; }
			float W[12];
			aff(W, FP[b], m->pal[k].m);
			for (int r = 0; r < 3; r++) {
				C3D_FVec* u = &U[k * 3 + r];
				u->x = W[r * 4]; u->y = W[r * 4 + 1]; u->z = W[r * 4 + 2]; u->w = W[r * 4 + 3];
			}
		}
		C3D_FVUnifSet(GPU_VERTEX_SHADER, sMcol, m->col[0], m->col[1], m->col[2], 1);
		if (m->tex != lastTex) {
			if (m->tex < ntex) C3D_TexBind(0, &texs[m->tex]);
			lastTex = m->tex;
		}
		if (m->alpha != lastAlpha) {
			C3D_AlphaTest(m->alpha != 0, GPU_GREATER, 127);
			lastAlpha = m->alpha;
		}
		C3D_SetBufInfo(&m->buf);
		C3D_DrawElements(GPU_TRIANGLES, m->nidx, C3D_UNSIGNED_SHORT, m->idx);
		statDraws++;
		statTris += m->nidx / 3;
	}
}

// All actors (the player but in FPP), then the world's program back.
static void drawActors(const C3D_Mtx* mvp, const float fwd[3])
{
	if (!chrPlaced) return;
	if (!chOk) { if (camMode != CAM_FPP) drawBoxes(); return; }
	u64 t0 = ticks();
	float pl[4][4];
	for (int i = 0; i < 4; i++) {
		int row = i >> 1;
		float sgn = (i & 1) ? -1.0f : 1.0f;
		pl[i][0] = mvp->r[3].x + sgn * mvp->r[row].x;
		pl[i][1] = mvp->r[3].y + sgn * mvp->r[row].y;
		pl[i][2] = mvp->r[3].z + sgn * mvp->r[row].z;
		pl[i][3] = mvp->r[3].w + sgn * mvp->r[row].w;
		float l = sqrtf(pl[i][0] * pl[i][0] + pl[i][1] * pl[i][1] + pl[i][2] * pl[i][2]);
		for (int j = 0; j < 4; j++) pl[i][j] /= l;
	}
	skinProgram(mvp, fwd);
	for (int i = 0; i < MAX_ACTORS; i++) {
		if (!actors[i].used) continue;
		if (i == 0 && camMode == CAM_FPP) continue;
		drawActor(&actors[i], pl);
	}
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_BindProgram(&prog);
	C3D_SetAttrInfo(&attr);
	chDrawMs += msSince(t0);
}

// L+A: five more NPCs round the player, in random clothes, some armed, idling (each its own phase).
static void spawnNpcs(int n)
{
	for (int k = 0; k < n; k++) {
		int slot = -1;
		for (int i = 1; i < MAX_ACTORS; i++) if (!actors[i].used) { slot = i; break; }
		if (slot < 0) return;
		Actor* a = &actors[slot];
		memset(a, 0, sizeof *a);
		float ang = (nNpc * 2.39996f), d = 140.0f + 45.0f * sqrtf((float)nNpc + 1);
		a->pos[0] = chr[0] + sinf(ang) * d; a->pos[1] = chr[1] + cosf(ang) * d;
		float g;
		// where it stands: the ground, or a floor at most a step over the player's feet (not the shacks'
		// planks 400 up: the user saw an NPC standing in the air)
		if (!standAt(a->pos[0], a->pos[1], chr[2], 45.0f, &g) || g < -9000.0f) g = chr[2];
		if (g < -SWIM_DEPTH) g = chr[2];
		a->pos[2] = g;
		a->yaw = atan2f(chr[0] - a->pos[0], chr[1] - a->pos[1]);
		u32 r = (u32)(nNpc * 2654435761u + 12345u);
		a->outfit = chNOutfit ? (int)((r >> 8) % chNOutfit) : 0;
		a->helmet = (r >> 4) & 1;
		a->weapon = chNWeapon && ((r >> 12) % 3 == 0) ? (int)((r >> 16) % chNWeapon) : -1;
		a->cur = -1; a->prev = -1; a->t = (r % 100) / 37.0f;
		a->act = -1; a->hp = a->maxHp = 50;
		a->used = true; a->dirty = true;
		nNpc++;
	}
}

static void clearNpcs(void)
{
	for (int i = 1; i < MAX_ACTORS; i++) actors[i].used = false;
	nNpc = 0;
}

static void setCamLimits(void);

// ------------------------------------------------------------------ turn-based fight (1:1)

// "Walka": the nearest NPC (or a new one in front) faces the player and they take turns. The
// player picks one of Morrowind's three attacks (slash: sure, medium; thrust: surest, weakest; chop:
// the strongest, misses most), the swing plays, at its hit key the roll: a hit takes the damage
// (the weapon's, less the armour's share) and the other one flinches or dies. Then the NPC's turn.
enum { F_CHOOSE, F_PLAYER, F_WAIT, F_ENEMY, F_OVER };
static struct {
	bool on;
	int enemy, phase, kind;
	float t;
	bool resolved, won;
	char msg[64];
} fight;
static const char* const ATK_NAMES[3] = { "Ciecie", "Pchniecie", "Rabanie" };
static const float ATK_HIT[3] = { 0.75f, 0.85f, 0.60f }, ATK_DMG[3] = { 1.0f, 0.8f, 1.35f };

static float armourOf(const Actor* a)
{
	if (a->custom) {   // 4 % per piece of armour worn (not clothes), at most 40 %
		int n = 0;
		for (int k = 0; k < 64 && k < chNItem; k++)
			if (a->worn[k] && !strstr(chItem[k].name, "common")) n++;
		return n * 0.04f > 0.4f ? 0.4f : n * 0.04f;
	}
	static const float BY[6] = { 0, 0, 0.15f, 0.3f, 0.3f, 0.4f };   // bare, commoner, netch, iron, bonemold, glass
	return a->outfit < 6 ? BY[a->outfit] : 0.2f;
}

static int atkAnim(const Actor* a, int kind)
{
	return (a->weapon >= 0 ? A_SLASH1H : A_SLASHHH) + kind;
}

static float hitTime(int animK)
{
	int an = chA[animK];
	return an >= 0 && chAnim[an].speed > 0 && chAnim[an].speed < 3 ? chAnim[an].speed : 0.35f;
}

static void faceEach(void)
{
	Actor* e = &actors[fight.enemy];
	chrYaw = atan2f(e->pos[0] - chr[0], e->pos[1] - chr[1]);
	e->yaw = atan2f(chr[0] - e->pos[0], chr[1] - e->pos[1]);
}

static void fightStartAt(int want)
{
	if (!chOk || swimming) return;
	int best = -1; float bd = 600.0f * 600.0f;
	if (want > 0 && want < MAX_ACTORS && actors[want].used && !actors[want].dead) { best = want; bd = 0; }
	for (int i = 1; i < MAX_ACTORS && best != want; i++) {
		Actor* a = &actors[i];
		if (!a->used || a->dead) continue;
		float dx = a->pos[0] - chr[0], dy = a->pos[1] - chr[1];
		if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = i; }
	}
	if (best < 0) {   // nobody near: one comes, armed
		spawnNpcs(1);
		for (int i = MAX_ACTORS - 1; i > 0; i--) if (actors[i].used && !actors[i].dead) { best = i; break; }
		if (best < 0) return;
		if (actors[best].weapon < 0 && chNWeapon) { actors[best].weapon = 0; actors[best].dirty = true; }
	}
	Actor* e = &actors[best];
	// 110 units in front of the player
	float fy = sinf(chrYaw), fx2 = cosf(chrYaw);
	float dx = e->pos[0] - chr[0], dy = e->pos[1] - chr[1], d = sqrtf(dx * dx + dy * dy);
	if (d < 1) { dx = fy; dy = fx2; d = 1; }
	e->pos[0] = chr[0] + dx / d * 110.0f; e->pos[1] = chr[1] + dy / d * 110.0f;
	float g;
	if (standAt(e->pos[0], e->pos[1], chr[2], 45.0f, &g) && g > -SWIM_DEPTH) e->pos[2] = g; else e->pos[2] = chr[2];
	e->hp = e->maxHp; e->act = -1;
	memset(&fight, 0, sizeof fight);
	fight.on = true; fight.enemy = best; fight.phase = F_CHOOSE;
	snprintf(fight.msg, sizeof fight.msg, "Twoj ruch: wybierz cios");
	faceEach();
	logw("    fight: player %.0f %.0f %.0f, enemy #%d %.0f %.0f %.0f\n", chr[0], chr[1], chr[2], best, e->pos[0], e->pos[1], e->pos[2]);
	if (camMode == CAM_FPP) camMode = CAM_TPP;
	setCamLimits();
}

static void fightStart(void) { fightStartAt(-1); }

static void fightEnd(void)
{
	if (fight.on && fight.enemy > 0 && !actors[fight.enemy].dead) actors[fight.enemy].act = -1;
	fight.on = false;
}

// The roll of an attack of `by` on `on`.
static void fightResolve(Actor* by, Actor* on, bool playerHits)
{
	int k = fight.kind;
	float r = (rand() % 1000) / 1000.0f;
	if (r > ATK_HIT[k]) {
		audioPlay(SND_MISS, 0.8f, 0);
		snprintf(fight.msg, sizeof fight.msg, "%s %s: pudlo!", playerHits ? "Twoje" : "Jego", ATK_NAMES[k]);
		return;
	}
	int base = by->weapon >= 0 ? 7 + rand() % 7 : 2 + rand() % 4;
	float arm = armourOf(on);
	int dmg = (int)(base * ATK_DMG[k] * (1.0f - arm) + 0.5f);
	if (dmg < 1) dmg = 1;
	on->hp -= dmg;
	audioPlay(by->weapon >= 0 ? (arm > 0.1f ? SND_HITARMOUR : SND_HITBODY) : SND_PUNCH, 0.9f, 0);
	if (on->hp <= 0) {
		on->hp = 0; on->dead = true; on->act = A_DEATH; on->t = 0;
		audioPlay(SND_FALL, 0.8f, 0);
		snprintf(fight.msg, sizeof fight.msg, "%s: %d obrazen - %s", ATK_NAMES[k], dmg, playerHits ? "przeciwnik pada!" : "padasz...");
	} else {
		on->act = A_HIT; on->t = 0;
		snprintf(fight.msg, sizeof fight.msg, "%s %s: trafienie, %d obrazen", playerHits ? "Twoje" : "Jego", ATK_NAMES[k], dmg);
	}
}

// The player's choice (0 slash, 1 thrust, 2 chop) on its turn.
static void fightAttack(int kind)
{
	if (!fight.on || fight.phase != F_CHOOSE) return;
	fight.kind = kind; fight.phase = F_PLAYER; fight.t = 0; fight.resolved = false;
	PL->act = atkAnim(PL, kind); PL->t = 0;
	audioPlay((rand() & 1) ? SND_SWING1 : SND_SWING2, 0.8f, 0);
	snprintf(fight.msg, sizeof fight.msg, "%s...", ATK_NAMES[kind]);
}

static bool fightUpdate(float dt)   // true: the window must be drawn again
{
	if (!fight.on) return false;
	Actor* e = &actors[fight.enemy];
	if (!e->used) { fight.on = false; return true; }
	faceEach();
	fight.t += dt;
	int ph = fight.phase;
	if (ph == F_PLAYER || ph == F_ENEMY) {
		Actor* by = ph == F_PLAYER ? PL : e;
		Actor* on = ph == F_PLAYER ? e : PL;
		int anim = atkAnim(by, fight.kind);
		if (!fight.resolved && fight.t >= hitTime(anim)) { fightResolve(by, on, ph == F_PLAYER); fight.resolved = true; return true; }
		if (fight.t >= animLength(chA[anim] >= 0 ? chA[anim] : 0) + 0.4f) {
			if (on->dead) { fight.phase = F_OVER; fight.won = ph == F_PLAYER; fight.t = 0; }
			else if (ph == F_PLAYER) { fight.phase = F_WAIT; fight.t = 0; snprintf(fight.msg, sizeof fight.msg, "Ruch przeciwnika..."); }
			else { fight.phase = F_CHOOSE; fight.t = 0; snprintf(fight.msg, sizeof fight.msg, "Twoj ruch: wybierz cios"); }
			return true;
		}
	} else if (ph == F_WAIT && fight.t > 0.7f) {   // the NPC picks an attack
		fight.kind = rand() % 3; fight.phase = F_ENEMY; fight.t = 0; fight.resolved = false;
		e->act = atkAnim(e, fight.kind); e->t = 0;
		audioPlay((rand() & 1) ? SND_SWING1 : SND_SWING2, 0.7f, 0.3f);
		snprintf(fight.msg, sizeof fight.msg, "Przeciwnik: %s...", ATK_NAMES[fight.kind]);
		return true;
	} else if (ph == F_OVER && fight.t > 3.0f) {
		if (!fight.won) {   // the player gets up again
			PL->dead = false; PL->act = -1; PL->hp = PL->maxHp;
			e->act = -1; e->hp = e->maxHp;
		}
		fight.on = false;
		return true;
	} else if (ph == F_OVER && fight.t < dt * 1.5f) {
		snprintf(fight.msg, sizeof fight.msg, fight.won ? "Zwyciestwo!" : "Porazka... (wstajesz za chwile)");
		return true;
	}
	return false;
}

// The inventory (X): on the bottom screen, the D-pad's up / down choose, A takes / puts on.
// The entries: the outfits, the helmet, bare hands, the weapons, the torch.
static bool invOpen, invDirty;
static bool useConsole;      // the benchmark's text console instead of the window
static UiState uiState;
static int uiSel = -1;

static int invCount(void);
static int invItems(void);
static void invLabel(int i, char* out, int n, bool* on);

// The window's contents from the player's state.
static void uiFill(void)
{
	UiState* u = &uiState;
	static char iconName[UI_MAXITEMS][24];
	u->nitems = 0;
	int total = invCount(), ni = invItems();
	u->npages = (total + UI_MAXITEMS - 1) / UI_MAXITEMS;
	if (u->page >= u->npages) u->page = u->npages - 1;
	if (u->page < 0) u->page = 0;
	for (int j = 0; j < UI_MAXITEMS; j++) {
		int i = u->page * UI_MAXITEMS + j;
		if (i >= total) break;
		bool on;
		invLabel(i, u->item[j].label, sizeof u->item[j].label, &on);
		u->item[j].on = on;
		if (i < ni) snprintf(iconName[j], 24, "item%d", i);
		else if (i == ni) snprintf(iconName[j], 24, "hands");
		else if (i < ni + 1 + chNWeapon) snprintf(iconName[j], 24, "weapon%d", i - ni - 1);
		else snprintf(iconName[j], 24, "torch");
		u->item[j].icon = iconName[j];
		u->nitems++;
	}
	u->selected = uiSel - u->page * UI_MAXITEMS;
	u->camName = rtsMode ? CAM_NAMES[camMode] : "wolna";
	u->torch = torchOn;
	u->combat = fight.on;
	// what A does to the object in reach, or the last message (a script's, "taken: ...")
	if (noteText[0]) snprintf(u->note, sizeof u->note, "%s", noteText);
	else if (objTarget >= 0) {
		int ty = objType(objTarget);
		const char* verb = ty == OT_ITEM ? "Weź" : ty == OT_DOOR && objDest(objTarget) ? "Wejdź"
		                 : ty == OT_DOOR ? ((objs[objTarget].flags & OF_OPEN) ? "Zamknij" : "Otwórz")
		                 : ((objs[objTarget].flags & OF_OPEN) ? "Zamknij" : "Przeszukaj");
		snprintf(u->note, sizeof u->note, "A: %s - %s", verb, objName(objTarget));
	} else u->note[0] = 0;
	if (fight.on) {
		const Actor* e = &actors[fight.enemy];
		u->hpP = PL->hp; u->maxP = PL->maxHp; u->hpE = e->hp; u->maxE = e->maxHp;
		snprintf(u->enemy, sizeof u->enemy, "Przeciwnik (%s)", e->weapon >= 0 ? chWeapon[e->weapon].name : "piesci");
		snprintf(u->msg, sizeof u->msg, "%s", fight.msg);
		u->myTurn = fight.phase == F_CHOOSE;
	}
}
static int invSel;

static int invCount(void) { return (chNItem < 64 ? chNItem : 64) + 1 + chNWeapon + 1; }
static int invItems(void) { return chNItem < 64 ? chNItem : 64; }

static void invLabel(int i, char* out, int n, bool* on)
{
	*on = false;
	if (i < invItems()) {   // the id with spaces for underscores
		snprintf(out, n, "%s", chItem[i].name);
		for (char* c = out; *c; c++) if (*c == '_') *c = ' ';
		*on = PL->worn[i];
		return;
	}
	i -= invItems();
	if (i == 0) { snprintf(out, n, "gole rece"); *on = PL->weapon < 0; return; }
	i -= 1;
	if (i < chNWeapon) { snprintf(out, n, "%s", chWeapon[i].name); *on = PL->weapon == i; return; }
	snprintf(out, n, "torch"); *on = torchOn;
}

static void invApply(int i)
{
	int ni = invItems();
	int snd = i < ni ? (strstr(chItem[i].name, "common") ? SND_CLOTHES : SND_ARMOUR)
	        : i < ni + 1 + chNWeapon ? SND_BLADEOUT : SND_CLICK;
	audioPlay(snd, 0.7f, 0);
	if (i < ni) {   // a piece on (off whatever covers the same slots) or off
		if (PL->worn[i]) PL->worn[i] = 0;
		else {
			const CItem* it = &chItem[i];
			for (int k = 0; k < ni; k++) {
				if (!PL->worn[k] || k == i) continue;
				const CItem* o = &chItem[k];
				bool clash = false;
				for (int a = 0; a < it->n && !clash; a++)
					for (int b = 0; b < o->n; b++) if (it->parts[a * 4] == o->parts[b * 4]) { clash = true; break; }
				if (clash) PL->worn[k] = 0;
			}
			PL->worn[i] = 1;
		}
	}
	else if (i == ni) PL->weapon = -1;
	else if (i < ni + 1 + chNWeapon) PL->weapon = i - ni - 1;
	else torchOn = !torchOn;
	PL->dirty = true;
	invDirty = true;
}

static void invDraw(void)
{
	printf("\x1b[1;0H\x1b[33m  INVENTORY (X close, A use)     \x1b[0m\n\n");
	for (int i = 0; i < invCount() && i < 22; i++) {
		char b[40];
		bool on;
		invLabel(i, b, sizeof b, &on);
		printf("%s %c %-26s\x1b[0m\n", i == invSel ? "\x1b[7m>" : " ", on ? '*' : ' ', b);
	}
	printf("\n NPCs: %d (L+A more, L+Y none)   \n", nNpc);
	invDirty = false;
}

// ------------------------------------------------------------------ water, shadow

// Tiling value noise: a lattice of gx x gy random values (powers of 2) over the texture,
// smoothly interpolated; whole lattices, so the texture repeats without a seam.
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

// octaves: lattice sizes from (gx0, gy0) doubling, amplitudes from 1 by `gain`; result 0..1
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

// Writes the levels of a mipmapped L8 (A == NULL) or LA8 texture from linear 0..1 arrays
// (they are averaged down in place), into the GPU's 8 x 8 tiled order.
static u32 morton8(int x, int y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static void writeLevels(C3D_Tex* t, int N, float* L, float* A)
{
	u8* dst = (u8*)t->data;
	int bpp = A ? 2 : 1, w = N;
	while (true) {
		for (int y = 0; y < w; y++)
			for (int x = 0; x < w; x++) {
				int o = ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton8(x & 7, y & 7);
				int l = (int)(L[y * w + x] * 255.0f + 0.5f);
				l = l < 0 ? 0 : l > 255 ? 255 : l;
				if (!A) dst[o] = (u8)l;
				else {
					int a = (int)(A[y * w + x] * 255.0f + 0.5f);
					a = a < 0 ? 0 : a > 255 ? 255 : a;
					dst[o * 2] = (u8)a; dst[o * 2 + 1] = (u8)l;   // LA8: alpha first, then luminance
				}
			}
		dst += w * w * bpp;
		if (w <= 8) break;
		int h = w / 2;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < h; x++) {
				int i0 = 2 * y * w + 2 * x;
				L[y * h + x] = (L[i0] + L[i0 + 1] + L[i0 + w] + L[i0 + w + 1]) * 0.25f;
				if (A) A[y * h + x] = (A[i0] + A[i0 + 1] + A[i0 + w] + A[i0 + w + 1]) * 0.25f;
			}
		w = h;
	}
	C3D_TexFlush(t);
	C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(t, GPU_LINEAR);
	C3D_TexSetWrap(t, GPU_REPEAT, GPU_REPEAT);
}

static float noiseBuf[256 * 256], noiseBuf2[256 * 256];

// The terrain's detail (L8 128, repeated every 256 units, x2 over the colour): a fine grain
// round mid grey, its mean exactly mid grey so that the far mip levels leave the colour as it is.
static void makeDetail(void)
{
	const int N = 128;
	if (!C3D_TexInitMipmap(&detailTex, N, N, GPU_L8)) return;
	fbm(noiseBuf, N, 8, 8, 4, 0.6f, 777);
	float mean = 0;
	for (int i = 0; i < N * N; i++) mean += noiseBuf[i];
	mean /= N * N;
	for (int i = 0; i < N * N; i++) noiseBuf[i] = 0.5f + (noiseBuf[i] - mean) * 0.30f;
	writeLevels(&detailTex, N, noiseBuf, NULL);
	detailOk = true;
}

// The ripples (LA8 256, tiling): crests of stretched noise (1 - |2n - 1|: lines where the noise
// crosses its middle), low contrast round mid grey; the alpha marks the tops of the crests, and
// where the two scrolled layers' tops meet the sun glints.
static void makeRipples(void)
{
	const int N = 256;
	if (!C3D_TexInitMipmap(&rippleTex, N, N, GPU_LA8)) return;
	fbm(noiseBuf, N, 8, 4, 4, 0.55f, 4242);
	float mean = 0;
	for (int i = 0; i < N * N; i++) {
		float r = 1 - fabsf(2 * noiseBuf[i] - 1);
		noiseBuf[i] = r;
		mean += r;
	}
	mean /= N * N;
	for (int i = 0; i < N * N; i++) {
		float r = noiseBuf[i];
		float g = (r - 0.8f) / 0.2f;
		g = g < 0 ? 0 : g * g;
		noiseBuf2[i] = g * 0.45f;   // the glints: softer (hardware test 9: too white)
		noiseBuf[i] = 0.5f + (r - mean) * 0.22f;
	}
	writeLevels(&rippleTex, N, noiseBuf, noiseBuf2);
	rippleOk = true;
}

static void waterProgram(const C3D_Mtx* mvp, const float fwd[3])
{
	C3D_BindProgram(&wProg);
	C3D_SetAttrInfo(&attr);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, wMvp, mvp);
	float fs = farDist * fogStart, fe = farDist, k = 1.0f / (fe - fs);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wFog, -fwd[0] * k, -fwd[1] * k, -fwd[2] * k, fe * k);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wFres, 0, 0, 0, 0);   // no fresnel (shadow, caustics)
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wRefl, 0, 0, 0, 0);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
}

// A soft round shadow under the character: a fan laid on the ground, dark in the middle.
static Vtx* shV;
static u16* shI;
static C3D_BufInfo shBuf;
#define SH_SEG 16

static void drawShadow(const C3D_Mtx* mvp, const float fwd[3])
{
	if (!shV || !chrPlaced) return;
	waterProgram(mvp, fwd);
	for (int i = 0; i <= SH_SEG; i++) {
		float x = 0, y = 0, a = 120;
		if (i > 0) {
			float ang = (i - 1) * 2.0f * (float)M_PI / SH_SEG;
			x = cosf(ang) * 42.0f; y = sinf(ang) * 34.0f;
			float cy = cosf(chrYaw), sy = sinf(chrYaw);   // a little longer along the facing
			float rx = x * cy + y * sy, ry = -x * sy + y * cy;
			x = rx; y = ry; a = 0;
		}
		float g = chr[2];
		groundAt(chr[0] + x, chr[1] + y, &g);
		if (g < -9000.0f) g = chr[2];
		Vtx* v = &shV[i];
		v->p[0] = (s16)lrintf(x); v->p[1] = (s16)lrintf(y); v->p[2] = (s16)lrintf(g - chr[2] + 3.0f); v->p[3] = 0;
		v->t[0] = v->t[1] = 0;
		v->c[0] = v->c[1] = v->c[2] = 0; v->c[3] = (u8)a;
	}
	GSPGPU_FlushDataCache(shV, (SH_SEG + 1) * sizeof(Vtx));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wOrigin, chr[0] - eye[0], chr[1] - eye[1], chr[2] - eye[2], 0);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	env = C3D_GetTexEnv(2);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_SetBufInfo(&shBuf);
	C3D_DrawElements(GPU_TRIANGLES, SH_SEG * 3, C3D_UNSIGNED_SHORT, shI);
	statDraws++;
	statTris += SH_SEG;
}

// The torch's flame: two fans facing the camera, added (a bright yellow core, an orange halo),
// their size flickering with the light.
#define FL_SEG 10
static Vtx* flV;
static u16* flI;
static C3D_BufInfo flBuf;

static void drawFlame(const C3D_Mtx* mvp, const float fwd[3])
{
	if (!flV || !chrPlaced) return;
	waterProgram(mvp, fwd);
	float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
	float rx = cy, ry = -sy, ux = -sy * sp, uy = -cy * sp, uz = cp;   // the camera's right and up
	for (int f = 0; f < 2; f++) {
		float r = (f == 0 ? 7.0f : 30.0f) * (0.85f + 0.15f * torchFlicker);
		float stretch = f == 0 ? 1.8f : 1.2f;   // flames are taller than wide
		const u8 core[2][4] = { { 255, 235, 150, 230 }, { 255, 140, 40, 110 } };
		for (int i = 0; i <= FL_SEG; i++) {
			Vtx* v = &flV[f * (FL_SEG + 1) + i];
			float a = 0, b = 0;
			if (i > 0) { float t = (i - 1) * 2.0f * (float)M_PI / FL_SEG; a = cosf(t) * r; b = sinf(t) * r * stretch + (f == 0 ? r * 0.5f : 0); }
			v->p[0] = (s16)lrintf(rx * a + ux * b); v->p[1] = (s16)lrintf(ry * a + uy * b); v->p[2] = (s16)lrintf(uz * b); v->p[3] = 0;
			v->t[0] = v->t[1] = 0;
			v->c[0] = core[f][0]; v->c[1] = core[f][1]; v->c[2] = core[f][2]; v->c[3] = i == 0 ? core[f][3] : 0;
		}
	}
	GSPGPU_FlushDataCache(flV, 2 * (FL_SEG + 1) * sizeof(Vtx));
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wOrigin, torchPos[0] - eye[0], torchPos[1] - eye[1], torchPos[2] - eye[2], 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	for (int i = 0; i < 3; i++) {
		C3D_TexEnv* env = C3D_GetTexEnv(i);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, i == 0 ? GPU_PRIMARY_COLOR : GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	C3D_SetBufInfo(&flBuf);
	C3D_DrawElements(GPU_TRIANGLES, 2 * FL_SEG * 3, C3D_UNSIGNED_SHORT, flI);
	statDraws++;
	statTris += 2 * FL_SEG;
}

// Under the water, the surface seen from below: the grid again, a light blue-green, rippled.
static void drawWaterBelow(const C3D_Mtx* mvp, const float fwd[3])
{
	if (!rippleOk) return;
	waterProgram(mvp, fwd);
	float t = fxTime;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wUvA, fmodf(t * 0.031f, 1.0f), fmodf(t * 0.019f, 1.0f), 14.0f / 8192.0f, 0);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wUvB, fmodf(-t * 0.023f, 1.0f), fmodf(t * 0.034f, 1.0f), 11.0f / 8192.0f, 0);
	C3D_TexBind(0, &rippleTex);
	C3D_TexBind(1, &rippleTex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD_SIGNED);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvColor(env, 0xC0000000);
	env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
	C3D_TexEnvColor(env, 0xFF000000 | 0xA0 << 16 | 0x90 << 8 | 0x50);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	env = C3D_GetTexEnv(2);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int ci = 0; ci < MAX_CELLS; ci++) {
		Cell* c = &cells[ci];
		if (!c->used || !c->wv || !c->wni) continue;
		float ox = c->gx * CELL + HALF - eye[0], oy = c->gy * CELL + HALF - eye[1];
		if (fabsf(ox) - HALF > farDist || fabsf(oy) - HALF > farDist) continue;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, wOrigin, ox, oy, -eye[2], 0);
		C3D_SetBufInfo(&c->wbuf);
		C3D_DrawElements(GPU_TRIANGLES, c->wni, C3D_UNSIGNED_SHORT, c->wi);
		statDraws++;
		statTris += c->wni / 3;
	}
}

// The water grids: the depth colour x the two ripple layers (added signed, x2: mid grey leaves
// the colour as it is), the transparency from the depth, faded out by the fog.
// Caustics: the grid again, on the bottom (the terrain's own vertices, so the same depth:
// GEQUAL), the two layers' crest tops (alpha) added, x the strength, added to the colour.
static void drawCaustics(const C3D_Mtx* mvp, const float fwd[3])
{
	if (!rippleOk) return;
	waterProgram(mvp, fwd);
	float t = fxTime;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wUvA, fmodf(t * 0.045f, 1.0f), fmodf(-t * 0.027f, 1.0f), 12.0f / 8192.0f, 0);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wUvB, fmodf(-t * 0.031f, 1.0f), fmodf(t * 0.052f, 1.0f), 10.0f / 8192.0f, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
	C3D_TexBind(0, &rippleTex);
	C3D_TexBind(1, &rippleTex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE1, 0);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_ADD);
	env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	env = C3D_GetTexEnv(2);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int ci = 0; ci < MAX_CELLS; ci++) {
		Cell* c = &cells[ci];
		if (!c->used || !c->cv || !c->wni || !c->hgt) continue;
		float ox = c->gx * CELL + HALF - eye[0], oy = c->gy * CELL + HALF - eye[1];
		if (fabsf(ox) - HALF > farDist || fabsf(oy) - HALF > farDist) continue;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, wOrigin, ox, oy, -eye[2], 0);
		C3D_SetBufInfo(&c->cbuf);
		C3D_DrawElements(GPU_TRIANGLES, c->wni, C3D_UNSIGNED_SHORT, c->wi);
		statDraws++;
		statTris += c->wni / 3;
	}
}

static void drawWater(const C3D_Mtx* mvp, const float fwd[3])
{
	if (!rippleOk) return;
	waterProgram(mvp, fwd);
	// 1: the bottom filtered: dst x lerp(1, colour, fog) (the colour premultiplied by the fog alpha);
	// the fresnel hides it at a glancing angle
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wFres, 0.9f, 0, 0, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	for (int i = 1; i <= 2; i++) {
		env = C3D_GetTexEnv(i);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	for (int ci = 0; ci < MAX_CELLS; ci++) {
		Cell* c = &cells[ci];
		if (!c->used || !c->tv || !c->wni) continue;
		float ox = c->gx * CELL + HALF - eye[0], oy = c->gy * CELL + HALF - eye[1];
		if (fabsf(ox) - HALF > farDist || fabsf(oy) - HALF > farDist) continue;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, wOrigin, ox, oy, -eye[2], 0);
		C3D_SetBufInfo(&c->tbuf);
		C3D_DrawElements(GPU_TRIANGLES, c->wni, C3D_UNSIGNED_SHORT, c->wi);
		statDraws++;
		statTris += c->wni / 3;
	}
	// 2: the light scattered back and the sky mirrored (fresnel), x the ripples, + the glints:
	// added (x the fog alpha)
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wFres, 0, 0, 0, 0);
	// what it mirrors: the shores' dark greens, not the sky (= the fog's colour: the water looked
	// like haze, the user)
	const u8* R = TINTS[waterTint][1];
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wRefl, R[0] / 255.0f, R[1] / 255.0f, R[2] / 255.0f, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	float t = fxTime;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wUvA, fmodf(t * 0.031f, 1.0f), fmodf(t * 0.019f, 1.0f), 14.0f / 8192.0f, 0);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, wUvB, fmodf(-t * 0.023f, 1.0f), fmodf(t * 0.034f, 1.0f), 11.0f / 8192.0f, 0);
	C3D_TexBind(0, &rippleTex);
	C3D_TexBind(1, &rippleTex);
	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, 0);
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
	// sun glints (where both layers' crest tops meet, added white): off, the user liked the
	// water better without them (WATER_GLINTS 1 brings them back)
	env = C3D_GetTexEnv(2);
	C3D_TexEnvInit(env);
#define WATER_GLINTS 0
#if WATER_GLINTS
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MULTIPLY_ADD);
#else
	C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
#endif
	if (fxMode == FX_CALM) {   // the depth colour alone
		env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		env = C3D_GetTexEnv(1);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		env = C3D_GetTexEnv(2);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	for (int ci = 0; ci < MAX_CELLS; ci++) {
		Cell* c = &cells[ci];
		if (!c->used || !c->wv || !c->wni) continue;
		float ox = c->gx * CELL + HALF - eye[0], oy = c->gy * CELL + HALF - eye[1];
		if (fabsf(ox) - HALF > farDist || fabsf(oy) - HALF > farDist) continue;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, wOrigin, ox, oy, -eye[2], 0);
		C3D_SetBufInfo(&c->wbuf);
		C3D_DrawElements(GPU_TRIANGLES, c->wni, C3D_UNSIGNED_SHORT, c->wi);
		statDraws++;
		statTris += c->wni / 3;
	}
}

// ------------------------------------------------------------------ collision

// The baker's boxes (tools/mwbake.py collision_box): a footprint turned like the thing, between
// two heights. The character: a circle of CHR_R round its feet, its body from 25 (it steps over
// lower things) to 120 over the ground.
#define CHR_R 22.0f
static bool collideOn = true;

static bool inBox(const float* b, float x, float y, float z)
{
	if (b[7] < z + 25.0f || b[6] > z + 120.0f) return false;
	float dx = x - b[0], dy = y - b[1];
	float u = dx * b[2] + dy * b[3], w = -dx * b[3] + dy * b[2];
	float cu = u < -b[4] ? -b[4] : u > b[4] ? b[4] : u;
	float cw = w < -b[5] ? -b[5] : w > b[5] ? b[5] : w;
	return (u - cu) * (u - cu) + (w - cw) * (w - cw) < CHR_R * CHR_R;
}

// How much of the character's circle at (x, y) is in blocked squares (MWC3 cells: the baker's
// 16-unit grid of what stands in the body's height band over the ground): the number of them.
static int occOverlap(float x, float y, bool up)
{
	int n = 0;
	int x0 = (int)floorf((x - CHR_R) / 16.0f), x1 = (int)floorf((x + CHR_R) / 16.0f);
	int y0 = (int)floorf((y - CHR_R) / 16.0f), y1 = (int)floorf((y + CHR_R) / 16.0f);
	for (int sy = y0; sy <= y1; sy++)
		for (int sx = x0; sx <= x1; sx++) {
			int gx = sx >= 0 ? sx / 512 : -((-sx + 511) / 512), gy = sy >= 0 ? sy / 512 : -((-sy + 511) / 512);
			Cell* c = findCell(gx, gy);
			if (!c || !c->occ) continue;
			int lx = sx - gx * 512, ly = sy - gy * 512;
			if (!((up ? c->occU : c->occ)[(ly * 512 + lx) >> 3] & (1 << (lx & 7)))) continue;
			// the circle's distance to that square
			float qx = fmaxf(sx * 16.0f, fminf(x, sx * 16.0f + 16.0f)), qy = fmaxf(sy * 16.0f, fminf(y, sy * 16.0f + 16.0f));
			if ((qx - x) * (qx - x) + (qy - y) * (qy - y) < CHR_R * CHR_R) n++;
		}
	return n;
}

// Would the character at (x, y, z) stand in a thing? Where it is caught already (px, py) it may
// move as long as it gets no deeper in (so it can always walk out).
// Up on stairs or a roof (standing higher than the terrain there): the other blocked squares.
static bool upAt(float x, float y, float z)
{
	float t;
	return groundAt(x, y, &t) && z > t + 8.0f;
}

static bool collides(float x, float y, float z, float px, float py, float pz)
{
	if (!collideOn) return false;
	int nNew = occOverlap(x, y, upAt(x, y, z));
	if (nNew > 0) {
		int nOld = occOverlap(px, py, upAt(px, py, pz));
		if (nOld == 0 || nNew > nOld) return true;
	}
	for (int i = 0; i < MAX_CELLS; i++) {
		Cell* c = &cells[i];
		if (!c->used || !c->col || c->occ) continue;
		float ox = c->gx * CELL + HALF, oy = c->gy * CELL + HALF;
		float lx = x - ox, ly = y - oy;
		if (fabsf(lx) > HALF + 800.0f || fabsf(ly) > HALF + 800.0f) continue;
		float qx = px - ox, qy = py - oy;
		for (int k = 0; k < c->ncol; k++) {
			const float* b = c->col + k * 8;
			float r = (b[4] > b[5] ? b[4] : b[5]) + CHR_R;
			if (fabsf(lx - b[0]) > r * 1.5f || fabsf(ly - b[1]) > r * 1.5f) continue;
			if (inBox(b, lx, ly, z) && !inBox(b, qx, qy, z)) return true;
		}
	}
	return false;
}

// The world cursor (docs/PLAN.md, the user's design). FPP: a crosshair in the middle, the ray from the
// eyes; the name of what it hits is written under it (as Morrowind does), A uses that. RTS / TPP:
// SELECT shows Morrowind's mouse cursor, moved by the circle pad (at the screen's edge it turns the
// camera); the ray from the camera through it. A on the ground: the character walks there; on an
// object: walks up to it and uses it; on an NPC: walks up and talks (Y: walks up and fights).
// B or the circle pad (with the cursor off) stops the walk.
enum { CK_NONE, CK_GROUND, CK_OBJ, CK_NPC };
static bool curOn;                        // the cursor shown (RTS / TPP)
static float curX = 200.0f, curY = 150.0f;   // on the top screen (400 x 240)
static int curKind, curIdx;               // what the ray hits
static float curP[3], curDist;            // where, how far from the character
static char curLabel[80];
static struct {
	bool on;
	int kind, idx;                        // CK_GROUND: walk; CK_OBJ: use it; CK_NPC: talk (fight: true)
	bool fight;
	float p[3];
	float best, stuckT;                   // the nearest it got (to the next corner), how long without getting nearer
	float planned[3];                     // where the path was found to (an NPC moves on)
	int replans;
} walkTo;

static float rayClosest(const float o[3], const float d[3], const float c[3], float* along)
{
	float v[3] = { c[0] - o[0], c[1] - o[1], c[2] - o[2] };
	float t = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
	*along = t;
	float q[3] = { o[0] + d[0] * t - c[0], o[1] + d[1] * t - c[1], o[2] + d[2] * t - c[2] };
	return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}

// The surface (terrain, or a floor not over the point) under (x, y) seen from height z.
static float surfaceAt(float x, float y, float z, bool* ok)
{
	float g;
	*ok = groundAt(x, y, &g);
	if (!*ok) return -1e9f;
	float f = floorAt(x, y, z, 0);
	return f > g ? f : g;
}

// Is the 16-unit square under (x, y) blocked (the ground's map, or the one up on stairs / roofs)?
static bool occBit(float x, float y, bool up)
{
	int sx = (int)floorf(x / 16.0f), sy = (int)floorf(y / 16.0f);
	int gx = sx >= 0 ? sx / 512 : -((-sx + 511) / 512), gy = sy >= 0 ? sy / 512 : -((-sy + 511) / 512);
	Cell* c = findCell(gx, gy);
	if (!c || !c->occ) return false;
	int lx = sx - gx * 512, ly = sy - gy * 512;
	return ((up ? c->occU : c->occ)[(ly * 512 + lx) >> 3] >> (lx & 7)) & 1;
}

// The ray (o, unit d) against an object's own triangles: the distance to the first one hit, or 1e9.
static float rayObject(int i, const float o[3], const float d[3])
{
	const ObjModel* m = objModel(i);
	if (!m || !m->nparts) return 1e9f;
	float M[12];
	const float zero[3] = { 0, 0, 0 };
	objMatrix(i, zero, M);   // world = M v (+ the position in the last column)
	// into the model's space: R^T / s for M = R s (s: the scale x the quantisation, the same on every axis)
	float s2 = M[0] * M[0] + M[4] * M[4] + M[8] * M[8];
	if (s2 < 1e-12f) return 1e9f;
	float rel[3] = { o[0] - M[3], o[1] - M[7], o[2] - M[11] };
	float om[3], dm[3];
	for (int c = 0; c < 3; c++) {
		om[c] = (M[c] * rel[0] + M[4 + c] * rel[1] + M[8 + c] * rel[2]) / s2;
		dm[c] = (M[c] * d[0] + M[4 + c] * d[1] + M[8 + c] * d[2]) / s2;
	}
	// (o_m + t d_m is the world's o + t d: t stays in world units)
	float best = 1e9f;
	for (int k = 0; k < m->nparts; k++) {
		int pi = m->first + k;
		if (pi >= objNParts) break;
		const ObjPart* p = &objParts[pi];
		const s16* v = (const s16*)(objData + p->vofs);
		const u16* ix = (const u16*)(objData + p->iofs);
		for (u32 t = 0; t + 2 < p->icount; t += 3) {
			const s16* a = v + ix[t] * 8, *b = v + ix[t + 1] * 8, *c = v + ix[t + 2] * 8;
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float pv[3] = { dm[1] * e2[2] - dm[2] * e2[1], dm[2] * e2[0] - dm[0] * e2[2], dm[0] * e2[1] - dm[1] * e2[0] };
			float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
			if (fabsf(det) < 1e-9f) continue;
			float inv = 1.0f / det;
			float tv[3] = { om[0] - a[0], om[1] - a[1], om[2] - a[2] };
			float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
			if (u < 0 || u > 1) continue;
			float q[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
			float w = (dm[0] * q[0] + dm[1] * q[1] + dm[2] * q[2]) * inv;
			if (w < 0 || u + w > 1) continue;
			float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
			if (tt > 1.0f && tt < best) best = tt;
		}
	}
	return best;
}

// The ray against a standing body: an upright cylinder (radius 26, the feet to 130 up; lying dead: low).
static float rayActor(const Actor* a, const float o[3], const float d[3])
{
	float R = a->dead ? 45.0f : 26.0f, top = a->dead ? 30.0f : 130.0f;
	float px = o[0] - a->pos[0], py = o[1] - a->pos[1];
	float A = d[0] * d[0] + d[1] * d[1], B = px * d[0] + py * d[1], C = px * px + py * py - R * R;
	float best = 1e9f;
	if (A > 1e-6f) {
		float disc = B * B - A * C;
		if (disc >= 0) {
			float t = (-B - sqrtf(disc)) / A;
			float z = o[2] + d[2] * t - a->pos[2];
			if (t > 1.0f && z >= 0 && z <= top) best = t;
		}
	}
	if (best > 1e8f && fabsf(d[2]) > 1e-4f) {   // through the top (seen from above: the RTS camera)
		float t = (a->pos[2] + top - o[2]) / d[2];
		float x = o[0] + d[0] * t - a->pos[0], y = o[1] + d[1] * t - a->pos[1];
		if (t > 1.0f && x * x + y * y <= R * R) best = t;
	}
	return best;
}

// The ray from the camera (eye, its forward fwd) through the screen point (sx, sy): what it hits first.
// Precise: the terrain and the floors, the walls (the collision map's blocked squares in the body's band),
// the objects' own triangles, the NPCs' bodies.
static void cursorPick(const float fwd[3], float sx, float sy, float fov)
{
	float up0[3] = { 0, 0, 1 };
	float r[3] = { fwd[1] * up0[2] - fwd[2] * up0[1], fwd[2] * up0[0] - fwd[0] * up0[2], fwd[0] * up0[1] - fwd[1] * up0[0] };
	float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	if (rl < 1e-4f) { r[0] = 1; r[1] = 0; r[2] = 0; rl = 1; }
	for (int i = 0; i < 3; i++) r[i] /= rl;
	float u[3] = { r[1] * fwd[2] - r[2] * fwd[1], r[2] * fwd[0] - r[0] * fwd[2], r[0] * fwd[1] - r[1] * fwd[0] };
	float th = tanf(fov * (float)M_PI / 360.0f);
	float nx = (sx - 200.0f) / 200.0f * th * (400.0f / 240.0f), ny = (120.0f - sy) / 120.0f * th;
	float d[3];
	for (int i = 0; i < 3; i++) d[i] = fwd[i] + r[i] * nx + u[i] * ny;
	float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	for (int i = 0; i < 3; i++) d[i] /= dl;
	const float MAXT = camMode == CAM_FPP ? 2500.0f : 4000.0f;
	// the ground (and the walls): steps along the ray, then halving the last one
	float tg = 1e9f, twall = 1e9f;
	{
		float prev = 0;
		for (float t = 8.0f; t < MAXT; t += 12.0f) {
			bool ok;
			float px = eye[0] + d[0] * t, py = eye[1] + d[1] * t, pz = eye[2] + d[2] * t;
			float s = surfaceAt(px, py, pz, &ok);
			if (!ok) break;
			if (pz <= s) {
				float a = prev, b = t;
				for (int k = 0; k < 6; k++) {
					float m = (a + b) / 2;
					float s2 = surfaceAt(eye[0] + d[0] * m, eye[1] + d[1] * m, eye[2] + d[2] * m, &ok);
					if (eye[2] + d[2] * m <= s2) b = m; else a = m;
				}
				tg = b;
				break;
			}
			// a wall: a blocked square with the ray in the body's band over where one stands there
			float g; bool up = groundAt(px, py, &g) && s > g + 8.0f;
			if (pz < s + 140.0f && pz > s + 20.0f && occBit(px, py, up)) { twall = t; break; }
			prev = t;
		}
	}
	if (tg > 1e8f && twall > 1e8f && d[2] < -0.05f && camMode != CAM_FPP) {   // not found (a cell not in): the plane of the feet
		float t = (chr[2] - eye[2]) / d[2];
		if (t > 0 && t < MAXT) tg = t;
	}
	int kind = tg < 1e8f ? CK_GROUND : CK_NONE, idx = -1;
	float best = fminf(tg, twall);
	for (int i = 1; i < MAX_ACTORS; i++) {
		Actor* a = &actors[i];
		if (!a->used) continue;
		float t = rayActor(a, eye, d);
		if (t < best) { best = t; kind = CK_NPC; idx = i; }
	}
	if (objsOk)
		for (int i = 0; i < nobj; i++) {
			WObj* o = &objs[i];
			if (o->flags & OF_TAKEN) continue;
			const ObjModel* mo = objModel(i);
			float rad = mo ? mo->radius * o->scale : 20.0f;
			float t, m = rayClosest(eye, d, o->pos, &t);   // the bounding sphere first
			if (m > rad || t + rad < 0 || t - rad > best) continue;
			float th2 = rayObject(i, eye, d);
			if (th2 < best) { best = th2; kind = CK_OBJ; idx = i; }
		}
	curKind = kind; curIdx = idx;
	if (kind == CK_GROUND) for (int i = 0; i < 3; i++) curP[i] = eye[i] + d[i] * best;
	else if (kind == CK_NPC) memcpy(curP, actors[idx].pos, sizeof curP);
	else if (kind == CK_OBJ) memcpy(curP, objs[idx].pos, sizeof curP);
	curDist = sqrtf((curP[0] - chr[0]) * (curP[0] - chr[0]) + (curP[1] - chr[1]) * (curP[1] - chr[1]));
	// the name shown under the cursor (FPP: only what is in reach, as in Morrowind)
	curLabel[0] = 0;
	float metres = curDist / 70.0f;   // (Morrowind: ~70 units to a metre)
	if (kind == CK_OBJ) {
		const char* n = objName(idx);
		if (!n[0]) n = objId(idx);
		int ty = objType(idx);
		if (ty == OT_DOOR && objDest(idx)) snprintf(curLabel, sizeof curLabel, "%s", objDest(idx)[0] ? objDest(idx) : "Na zewnątrz");
		else snprintf(curLabel, sizeof curLabel, "%s", n);
	} else if (kind == CK_NPC) snprintf(curLabel, sizeof curLabel, actors[idx].dead ? "Zwłoki" : "Mieszkaniec  (A: rozmowa, Y: walka)");
	else if (kind == CK_GROUND && camMode != CAM_FPP) snprintf(curLabel, sizeof curLabel, "A: idź tutaj (%.0f m)", metres);
	if (camMode == CAM_FPP && kind != CK_GROUND && curDist > 260.0f) curLabel[0] = 0;
}

// Path finding (RTS cursor walks), as Morrowind does it: its path grids (PGRD: the points the designers set
// in a cell, on stairs and docks too, each with its height, and the links between them; tools/pathbake.py
// -> pathgrid.bin). The way: straight when the goal is seen (the collision map along the line), else from
// a point of the grid seen from the character over the links to one that sees the goal (A* over ~1000
// points: a fraction of a millisecond). Where there is no grid (the wilds): A* over the baked collision
// map's 16-unit squares, a few milliseconds a frame (the user: the squares in one go froze the game for
// seconds on docks and stairs).
typedef struct { float p[3]; u32 first; u16 n, cell; } __attribute__((packed)) PgPoint;
static PgPoint* pgPts;
static u16* pgLinks;
static int pgN;

static void pgLoad(void)
{
	FILE* f = fopen(dataPath("pathgrid.bin"), "rb");
	if (!f) return;
	char magic[4]; u32 n = 0, nl = 0;
	if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "MWP1", 4) && fread(&n, 4, 1, f) == 1 && fread(&nl, 4, 1, f) == 1 && n < 60000) {
		pgPts = malloc(n * sizeof(PgPoint)); pgLinks = malloc((nl ? nl : 1) * sizeof(u16));
		if (pgPts && pgLinks && fread(pgPts, sizeof(PgPoint), n, f) == n && fread(pgLinks, 2, nl, f) == nl) pgN = n;
	}
	fclose(f);
	logw("pathgrid: %d points\n", pgN);
}

#define PMAXW 128
static float pathPts[PMAXW][3];
static int pathN, pathAt;
static float pathMs;

// Free for the body at (x, y) standing at z: the square and (wide) the 8 round it.
static bool pClear(float x, float y, float z, bool wide)
{
	float g;
	bool up = groundAt(x, y, &g) && z > g + 8.0f;
	if (occBit(x, y, up)) return false;
	if (!wide) return true;
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
			if ((dx || dy) && occBit(x + dx * 16.0f, y + dy * 16.0f, up)) return false;
	return true;
}

// The straight way from a to b walkable (every 12 units: steps of at most 45, nothing in the way)?
static bool pLine(const float a[3], const float b[3])
{
	float dx = b[0] - a[0], dy = b[1] - a[1];
	float len = sqrtf(dx * dx + dy * dy);
	int n = (int)(len / 12.0f) + 1;
	float z = a[2];
	for (int k = 1; k <= n; k++) {
		float t = (float)k / n;
		float x = a[0] + dx * t, y = a[1] + dy * t, g;
		if (!standAt(x, y, z, 45.0f, &g)) return false;
		if (g < -9000.0f) g = -2000.0f;
		if (g > z + 45.0f || g < -SWIM_DEPTH) return false;
		if (fabsf(g - (a[2] + (b[2] - a[2]) * t)) > 80.0f) return false;   // off an edge the way keeps to
		if (!pClear(x, y, g, false)) return false;
		z = g;
	}
	return true;
}

// ---- A* over the path grid
#define PGNEAR 6
static float* pgG;
static u16* pgPar;
static u8* pgSt;

// the nearest grid points (at most PGNEAR, within r) of p that a straight walk joins with p
static int pgNearSeen(const float p[3], float r, bool fromPoint, int* out, float* dist)
{
	int idx[24]; float dd[24]; int m = 0;
	for (int i = 0; i < pgN; i++) {
		float dx = pgPts[i].p[0] - p[0], dy = pgPts[i].p[1] - p[1], dz = pgPts[i].p[2] - p[2];
		if (fabsf(dz) > 300.0f) continue;
		float d = dx * dx + dy * dy;
		if (d > r * r) continue;
		// keep the 24 nearest (insertion)
		int k;
		if (m < 24) k = m++;
		else if (d < dd[23]) k = 23;
		else continue;
		while (k > 0 && dd[k - 1] > d) { dd[k] = dd[k - 1]; idx[k] = idx[k - 1]; k--; }
		dd[k] = d; idx[k] = i;
	}
	int n = 0;
	for (int k = 0; k < m && n < PGNEAR; k++) {
		const float* q = pgPts[idx[k]].p;
		if (fromPoint ? pLine(q, p) : pLine(p, q)) { out[n] = idx[k]; dist[n] = sqrtf(dd[k]); n++; }
		if (k >= 10 && n == 0) break;   // (the ten nearest not seen: it is no use)
	}
	return n;
}

static bool pgFind(const float from[3], const float goal[3])
{
	if (!pgN) return false;
	if (!pgG) { pgG = malloc(pgN * 4); pgPar = malloc(pgN * 2); pgSt = malloc(pgN); }
	if (!pgG || !pgPar || !pgSt) return false;
	int sN[PGNEAR], gN[PGNEAR]; float sD[PGNEAR], gD[PGNEAR];
	int ns = pgNearSeen(from, 900.0f, false, sN, sD);
	if (!ns) return false;
	int ng = pgNearSeen(goal, 900.0f, true, gN, gD);
	if (!ng) return false;
	memset(pgSt, 0, pgN);
	// open list: a small linear scan (the grid is ~1000 points)
	for (int k = 0; k < ns; k++) { pgG[sN[k]] = sD[k]; pgPar[sN[k]] = 0xFFFF; pgSt[sN[k]] = 1; }
	int found = -1; float foundCost = 1e30f;
	for (int it = 0; it < pgN; it++) {
		int best = -1; float bf = 1e30f;
		for (int i = 0; i < pgN; i++)
			if (pgSt[i] == 1) {
				float dx = pgPts[i].p[0] - goal[0], dy = pgPts[i].p[1] - goal[1];
				float f = pgG[i] + sqrtf(dx * dx + dy * dy);
				if (f < bf) { bf = f; best = i; }
			}
		if (best < 0 || bf >= foundCost) break;
		pgSt[best] = 2;
		for (int k = 0; k < ng; k++)
			if (gN[k] == best && pgG[best] + gD[k] < foundCost) { foundCost = pgG[best] + gD[k]; found = best; }
		const PgPoint* p = &pgPts[best];
		for (u32 l = 0; l < p->n; l++) {
			int j = pgLinks[p->first + l];
			if (j >= pgN || pgSt[j] == 2) continue;
			float dx = pgPts[j].p[0] - p->p[0], dy = pgPts[j].p[1] - p->p[1], dz = pgPts[j].p[2] - p->p[2];
			float g = pgG[best] + sqrtf(dx * dx + dy * dy + dz * dz);
			if (pgSt[j] == 1 && g >= pgG[j]) continue;
			pgG[j] = g; pgPar[j] = (u16)best; pgSt[j] = 1;
		}
	}
	if (found < 0) return false;
	// the points back to the start, then the goal
	int chain[PMAXW], n = 0;
	for (int i = found; i != 0xFFFF && n < PMAXW - 1; i = pgPar[i]) chain[n++] = i;
	pathN = 0;
	for (int k = n - 1; k >= 0; k--) memcpy(pathPts[pathN++], pgPts[chain[k]].p, 12);
	memcpy(pathPts[pathN++], goal, 12);
	// a point skipped when the one after it is seen straight (a few tries each: cheap)
	float cur[3] = { from[0], from[1], from[2] };
	int w = 0;
	for (int i = 0; i < pathN; ) {
		int far = i;
		for (int j = i + 1; j < pathN && j <= i + 3; j++) if (pLine(cur, pathPts[j])) far = j;
		memcpy(pathPts[w], pathPts[far], 12);
		memcpy(cur, pathPts[far], 12);
		w++; i = far + 1;
	}
	pathN = w; pathAt = 0;
	return true;
}

// ---- A* over the squares, a few milliseconds a frame (where there is no grid)
#define PN 160
static float pG[PN * PN], pZ[PN * PN];
static u16 pPar[PN * PN];
static u8 pState[PN * PN];   // 0 unseen, 1 open, 2 closed
typedef struct { float f; u16 i; } PHeap;
#define PHEAP 24576
static PHeap pHeap[PHEAP];
static int pHeapN;
static struct { bool on; int ox, oy, s, expanded, pass; float goal[3], near; } gj;

static void pPush(float f, u16 i)
{
	if (pHeapN >= PHEAP) return;
	int k = pHeapN++;
	while (k > 0) {
		int up = (k - 1) / 2;
		if (pHeap[up].f <= f) break;
		pHeap[k] = pHeap[up]; k = up;
	}
	pHeap[k].f = f; pHeap[k].i = i;
}

static u16 pPop(void)
{
	u16 top = pHeap[0].i;
	PHeap last = pHeap[--pHeapN];
	int k = 0;
	for (;;) {
		int c = 2 * k + 1;
		if (c >= pHeapN) break;
		if (c + 1 < pHeapN && pHeap[c + 1].f < pHeap[c].f) c++;
		if (pHeap[c].f >= last.f) break;
		pHeap[k] = pHeap[c]; k = c;
	}
	if (pHeapN > 0) pHeap[k] = last;
	return top;
}

static void gridRestart(void)
{
	memset(pState, 0, sizeof pState);
	pHeapN = 0;
	pG[gj.s] = 0; pZ[gj.s] = chr[2]; pPar[gj.s] = 0xFFFF; pState[gj.s] = 1;
	pPush(0, (u16)gj.s);
}

static bool gridBegin(const float goal[3], float near)
{
	float mx = (chr[0] + goal[0]) / 2, my = (chr[1] + goal[1]) / 2;
	gj.ox = (int)floorf(mx / 16.0f) - PN / 2; gj.oy = (int)floorf(my / 16.0f) - PN / 2;
	int sx = (int)floorf(chr[0] / 16.0f) - gj.ox, sy = (int)floorf(chr[1] / 16.0f) - gj.oy;
	int gx = (int)floorf(goal[0] / 16.0f) - gj.ox, gy = (int)floorf(goal[1] / 16.0f) - gj.oy;
	if (sx < 1 || sy < 1 || sx >= PN - 1 || sy >= PN - 1 || gx < 1 || gy < 1 || gx >= PN - 1 || gy >= PN - 1) return false;
	gj.s = sy * PN + sx; gj.expanded = 0; gj.pass = 0; gj.near = near;
	memcpy(gj.goal, goal, 12);
	gridRestart();
	gj.on = true;
	return true;
}

// 1 found (pathPts), -1 none, 0 not yet (call again next frame)
static int gridStep(float budgetMs)
{
	u64 t0 = ticks();
	int found = -1;
	float nearSq = gj.near * gj.near;
	bool wide = gj.pass == 0;
	int n = 0;
	while (pHeapN > 0 && gj.expanded < 12000) {
		if ((++n & 31) == 0 && msSince(t0) > budgetMs) { pathMs += msSince(t0); return 0; }
		int i = pPop();
		if (pState[i] == 2) continue;
		pState[i] = 2;
		gj.expanded++;
		int x = i % PN, y = i / PN;
		float wx = (gj.ox + x) * 16.0f + 8.0f, wy = (gj.oy + y) * 16.0f + 8.0f;
		float ddx = wx - gj.goal[0], ddy = wy - gj.goal[1];
		if (ddx * ddx + ddy * ddy <= nearSq) { found = i; break; }
		for (int k = 0; k < 8; k++) {
			static const s8 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
			int nx = x + DX[k], ny = y + DY[k];
			if (nx < 0 || ny < 0 || nx >= PN || ny >= PN) continue;
			int j = ny * PN + nx;
			if (pState[j] == 2) continue;
			float nwx = (gj.ox + nx) * 16.0f + 8.0f, nwy = (gj.oy + ny) * 16.0f + 8.0f, g;
			if (k >= 4 && (!pClear(nwx, wy, pZ[i], false) || !pClear(wx, nwy, pZ[i], false))) continue;   // not round a corner
			if (!standAt(nwx, nwy, pZ[i], 45.0f, &g)) continue;
			if (g < -9000.0f) g = -2000.0f;
			if (g > pZ[i] + 45.0f) continue;
			bool swim = g < -SWIM_DEPTH;
			float z = swim ? SWIM_Z : g;
			if (!swim && !pClear(nwx, nwy, g, wide)) continue;
			float ng = pG[i] + (k >= 4 ? 22.6f : 16.0f) * (swim ? 3.0f : 1.0f);
			if (pState[j] == 1 && ng >= pG[j]) continue;
			pG[j] = ng; pZ[j] = z; pPar[j] = (u16)i; pState[j] = 1;
			float hx = fabsf(nwx - gj.goal[0]), hy = fabsf(nwy - gj.goal[1]);
			pPush(ng + fmaxf(hx, hy) + 0.414f * fminf(hx, hy), (u16)j);
		}
	}
	pathMs += msSince(t0);
	if (found < 0) {
		if (gj.pass == 0) { gj.pass = 1; gridRestart(); return 0; }   // again with the square itself only (narrow doors)
		gj.on = false;
		logw("    path (squares): none, %d expanded, %.1f ms\n", gj.expanded, pathMs);
		return -1;
	}
	// back to the start; a corner where the way turns
	static u16 sq[PN * 4];
	int n2 = 0;
	for (int i = found; i != 0xFFFF && n2 < PN * 4; i = pPar[i]) sq[n2++] = (u16)i;
	pathN = 0;
	int lastDir = -99;
	for (int k = n2 - 2; k >= 0 && pathN < PMAXW - 1; k--) {
		int a = sq[k + 1], b = sq[k];
		int dir = (b % PN - a % PN) * 3 + (b / PN - a / PN);
		if (dir != lastDir && k != n2 - 2) {
			pathPts[pathN][0] = (gj.ox + a % PN) * 16.0f + 8.0f; pathPts[pathN][1] = (gj.oy + a / PN) * 16.0f + 8.0f; pathPts[pathN][2] = pZ[a];
			pathN++;
		}
		lastDir = dir;
	}
	int e = sq[0];
	pathPts[pathN][0] = (gj.ox + e % PN) * 16.0f + 8.0f; pathPts[pathN][1] = (gj.oy + e / PN) * 16.0f + 8.0f; pathPts[pathN][2] = pZ[e];
	pathN++; pathAt = 0;
	gj.on = false;
	logw("    path (squares): %d squares, %d corners, %d expanded, %.1f ms over frames\n", n2, pathN, gj.expanded, pathMs);
	return 1;
}

// The way to `goal` (near enough: within `near`). True: pathPts is set, or the search over the squares
// goes on in walkStep (gj.on). False: straight.
static bool pathFind(const float goal[3], float near)
{
	u64 t0 = ticks();
	pathN = pathAt = 0; gj.on = false; pathMs = 0;
	// the goal's point to reach: `near` short of it on the way from the character (a door, a chest: their
	// middle is in a blocked square)
	float dx = goal[0] - chr[0], dy = goal[1] - chr[1], d = sqrtf(dx * dx + dy * dy);
	float to[3] = { goal[0], goal[1], goal[2] };
	if (d > near && near > 20.0f) { to[0] -= dx / d * (near - 10.0f); to[1] -= dy / d * (near - 10.0f); }
	float g;
	if (standAt(to[0], to[1], goal[2] + 40.0f, 45.0f, &g) && g > -9000.0f) to[2] = g;
	if (d < 2500.0f && pLine(chr, to)) {
		memcpy(pathPts[0], to, 12); pathN = 1;
		logw("    path: straight, %.1f ms\n", msSince(t0));
		return true;
	}
	if (pgFind(chr, to)) {
		logw("    path (grid): %d points, %.1f ms\n", pathN, msSince(t0));
		return true;
	}
	pathMs = msSince(t0);
	if (gridBegin(goal, near)) { logw("    path: no grid here, squares (%.1f ms so far)\n", pathMs); return true; }
	logw("    path: none, straight\n");
	return false;
}

// How near the walk must get to what it goes to (the ground point, an NPC, an object: its edge).
static float walkReach(void)
{
	if (walkTo.kind == CK_GROUND) return 20.0f;
	if (walkTo.kind == CK_NPC) return 100.0f;
	const ObjModel* mo = objModel(walkTo.idx);
	float rad = mo ? mo->radius * objs[walkTo.idx].scale : 20.0f;
	return 90.0f + (rad < 80.0f ? rad * 0.5f : 40.0f);
}

static void walkPlan(void)
{
	if (!pathFind(walkTo.p, fmaxf(12.0f, walkReach() - 12.0f))) pathN = pathAt = 0;
	memcpy(walkTo.planned, walkTo.p, sizeof walkTo.planned);
	walkTo.best = 1e9f; walkTo.stuckT = 0;
}

static void walkStart(int kind, int idx, const float p[3], bool fightIt)
{
	walkTo.on = true; walkTo.kind = kind; walkTo.idx = idx; walkTo.fight = fightIt;
	memcpy(walkTo.p, p, sizeof walkTo.p);
	walkTo.replans = 0;
	walkPlan();
}

static void fightStartAt(int want);

// What the walk wants from the circle pad this frame (mx, my relative to the camera as chrUpdate
// takes them); false when it is over. At the end: the object used, the NPC talked to / fought.
static bool walkStep(float dt, float* mx, float* my, bool* run)
{
	if (!walkTo.on) return false;
	if (gj.on) {   // the way over the squares is being found: a few milliseconds a frame, standing
		int r = gridStep(3.0f);
		*mx = *my = 0; *run = false;
		if (r < 0) pathN = pathAt = 0;   // none: straight
		if (r != 0) { walkTo.best = 1e9f; walkTo.stuckT = 0; }
		return true;
	}
	if (walkTo.kind == CK_NPC) {   // it may move
		Actor* a = &actors[walkTo.idx];
		if (!a->used) { walkTo.on = false; return false; }
		memcpy(walkTo.p, a->pos, sizeof walkTo.p);
	}
	float dx = walkTo.p[0] - chr[0], dy = walkTo.p[1] - chr[1];
	float d = sqrtf(dx * dx + dy * dy);
	float reach = walkReach();
	if (d <= reach) {
		walkTo.on = false;
		if (walkTo.kind == CK_OBJ) {
			chrYaw = atan2f(dx, dy);
			objActivate(walkTo.idx);
		} else if (walkTo.kind == CK_NPC && !actors[walkTo.idx].dead) {
			chrYaw = atan2f(dx, dy);
			if (walkTo.fight) fightStartAt(walkTo.idx);
			else {
				Actor* a = &actors[walkTo.idx];
				a->yaw = atan2f(chr[0] - a->pos[0], chr[1] - a->pos[1]);
				gameMessage("Mieszkaniec: \"Tak, obcy?\"  (dialogi wkrótce)");
			}
		}
		return false;
	}
	// an NPC went on: the way again
	if (walkTo.kind == CK_NPC) {
		float mx2 = walkTo.p[0] - walkTo.planned[0], my2 = walkTo.p[1] - walkTo.planned[1];
		if (mx2 * mx2 + my2 * my2 > 150.0f * 150.0f) walkPlan();
	}
	// the next corner of the path (passed when near it), else straight at it
	float tx = walkTo.p[0], ty = walkTo.p[1];
	while (pathAt < pathN) {
		float cx = pathPts[pathAt][0] - chr[0], cy = pathPts[pathAt][1] - chr[1];
		if (cx * cx + cy * cy > 18.0f * 18.0f || pathAt == pathN - 1) break;
		pathAt++; walkTo.best = 1e9f; walkTo.stuckT = 0;
	}
	if (pathAt < pathN) {
		float cx = pathPts[pathAt][0] - chr[0], cy = pathPts[pathAt][1] - chr[1];
		if (pathAt < pathN - 1 || cx * cx + cy * cy > 18.0f * 18.0f) { tx = pathPts[pathAt][0]; ty = pathPts[pathAt][1]; }
	}
	float sdx = tx - chr[0], sdy = ty - chr[1], sd = sqrtf(sdx * sdx + sdy * sdy);
	if (sd < 1.0f) { sdx = dx; sdy = dy; sd = d; }
	// stuck (something in the way): the way found again, twice, then given up
	if (sd < walkTo.best - 2.0f) { walkTo.best = sd; walkTo.stuckT = 0; }
	else if ((walkTo.stuckT += dt) > 1.0f) {
		if (walkTo.replans++ < 2) { logw("    walk: stuck %.0f away, the way again\n", d); walkPlan(); return true; }
		walkTo.on = false; gameMessage("Nie da się tam przejść"); logw("    walk: stuck %.0f away\n", d); return false;
	}
	float ux = sdx / sd, uy = sdy / sd;
	float fx = sinf(rtsYaw), fy = cosf(rtsYaw);
	*my = ux * fx + uy * fy;
	*mx = ux * fy - uy * fx;
	*run = d > 500.0f;
	return true;
}

// Walking: the circle pad relative to the camera (FPP: forward is where it looks), the figure
// turns to where it goes. Shallow water is waded through, deeper the character swims (A dives).
// Floors (bridges, docks, stairs) are stood on; off their edge it falls.
static void chrUpdate(float dt, float mx, float my, bool run, bool dive, bool jump)
{
	float g;
	if (!chrPlaced) {
		if (standAt(chr[0], chr[1], chr[2] + chrPlaceUp, 0, &g)) { chr[2] = g > -10000.0f ? g : SWIM_Z; chrPlaced = true; chrPlaceUp = 400.0f; }
		return;
	}
	float len = sqrtf(mx * mx + my * my);
	if (len > 1) { mx /= len; my /= len; len = 1; }
	float fx = sinf(rtsYaw), fy = cosf(rtsYaw);   // the camera's forward on the ground; right = (fy, -fx)
	float dx = fx * my + fy * mx, dy = fy * my - fx * mx;
	float moved = 0;
	const float STEP = 45.0f;
	if (len > 0.05f) {
		float sp = (swimming ? (run ? 260.0f : 150.0f) : run ? RUN_SPEED : WALK_SPEED) * len;
		float step = sp * dt;
		float ux = dx / len, uy = dy / len;
		// blocked: slide along (turned 35 and 70 degrees either way, or only x, only y)
		float tries[7][2];
		int nt = 0;
		tries[nt][0] = ux; tries[nt++][1] = uy;
		const float ang[4] = { 0.61f, -0.61f, 1.22f, -1.22f };
		for (int i = 0; i < 4; i++) {
			float c = cosf(ang[i]), s = sinf(ang[i]);
			tries[nt][0] = ux * c - uy * s; tries[nt++][1] = ux * s + uy * c;
		}
		for (int t = 0; t < nt; t++) {
			float k = t == 0 ? 1.0f : t < 3 ? 0.8f : 0.5f;   // sliding is slower
			float tx = chr[0] + tries[t][0] * step * k, ty = chr[1] + tries[t][1] * step * k;
			float base = swimming ? fmaxf(chr[2], -10.0f) : chr[2];
			if (!standAt(tx, ty, base, STEP, &g)) continue;   // that cell is not in yet
			if (g < -9000.0f) g = -2000.0f;                    // the open sea
			if (g > base + STEP) continue;                     // a wall of ground (a cliff), too high a floor
			if (collides(tx, ty, fmaxf(g, chr[2] - 60.0f), chr[0], chr[1], chr[2])) continue;
			moved = sqrtf((tx - chr[0]) * (tx - chr[0]) + (ty - chr[1]) * (ty - chr[1]));
			chr[0] = tx; chr[1] = ty;
			break;
		}
		if (camMode != CAM_FPP) {   // turn towards the way it goes (the shorter way round)
			float want = atan2f(dx, dy), d = want - chrYaw;
			while (d > (float)M_PI) d -= 2 * (float)M_PI;
			while (d < -(float)M_PI) d += 2 * (float)M_PI;
			float k = dt * 10.0f; if (k > 1) k = 1;
			chrYaw += d * k;
		}
	}
	if (camMode == CAM_FPP) chrYaw = rtsYaw;
	// the height: on the ground / a floor, swimming in the deep, falling off an edge
	if (standAt(chr[0], chr[1], chr[2], STEP, &g)) {
		if (g < -9000.0f) g = -2000.0f;   // the open sea
		if (g < -SWIM_DEPTH && chr[2] < SWIM_Z + 60.0f) {   // the water is deep here: swim
			swimming = true;
			chrVz = 0;
			float want = dive ? g + 30.0f : SWIM_Z;
			if (want < g + 30.0f) want = g + 30.0f;
			float sp = (dive ? 160.0f : 110.0f) * dt;
			if (chr[2] > want) chr[2] = fmaxf(want, chr[2] - sp);
			else chr[2] = fminf(want, chr[2] + sp);
		} else {
			swimming = false;
			if (jump && chr[2] <= g + 1.0f && !chrAir) { chrVz = 400.0f; chr[2] = g + 2.0f; }   // a jump (~60 high)
			if (!chrAir && chrVz <= 0 && chr[2] > g + 1.0f && chr[2] < g + 30.0f) {   // a small step down: follow it smoothly
				float k = dt * 14.0f; if (k > 1) k = 1;
				chr[2] += (g - chr[2]) * k;
				if (chr[2] < g + 1.5f) chr[2] = g;
			} else if (chr[2] > g + 1.0f) {   // in the air: rising or falling
				chrAir = true;
				chrVz -= 1300.0f * dt;
				chr[2] += chrVz * dt;
				if (chr[2] <= g) { chr[2] = g; chrVz = 0; chrAir = false; }
				if (g < -SWIM_DEPTH && chr[2] < SWIM_Z) { chr[2] = SWIM_Z; chrVz = 0; chrAir = false; }   // into the water
			} else {   // on the ground; a step up eased in (stairs, planks)
				float k = dt * 16.0f; if (k > 1) k = 1;
				chr[2] = g - chr[2] > 2.0f ? chr[2] + (g - chr[2]) * k : g;
				if (chr[2] < g) chr[2] = fmaxf(chr[2], g - 25.0f);
				chrVz = 0; chrAir = false;
			}
		}
	}
	if (swimming) chrAir = false;
	// in the water the body tips (as OpenMW swims): head down diving, up rising, a little forward swimming
	float wantP = 0;
	if (swimming) wantP = dive ? -1.25f : chr[2] < SWIM_Z - 8.0f ? 0.45f : len > 0.05f ? -0.25f : 0;
	float kp = dt * 4.0f; if (kp > 1) kp = 1;
	chrPitch += (wantP - chrPitch) * kp;
	PL->speed = dt > 0 ? moved / dt : 0;
	PL->run = run;
	// the walk cycle follows the distance: a step per ~70 units walking, longer running
	chrPhase += moved * (float)M_PI / (run ? 110.0f : 70.0f);
	float want = moved > 0 ? 1.0f : 0.0f, k = dt * 6.0f; if (k > 1) k = 1;
	chrSwing += (want - chrSwing) * k;
	if (chrSwing < 0.02f && moved == 0) chrPhase = 0;
}

// The character's cameras. RTS: above and behind, looking down at RTS_PITCH from rtsDist. TPP:
// behind the back at head height, a little down. FPP: from the eyes (the figure not drawn).
static void rtsCamera(float fwd[3])
{
	float g;
	if (camMode == CAM_FPP) {
		float p = camPitch;
		fwd[0] = cosf(p) * sinf(rtsYaw); fwd[1] = cosf(p) * cosf(rtsYaw); fwd[2] = sinf(p);
		eye[0] = chr[0] + sinf(rtsYaw) * 8.0f; eye[1] = chr[1] + cosf(rtsYaw) * 8.0f;
		eye[2] = chr[2] + EYE_Z;
		yaw = rtsYaw; pitch = p;
		return;
	}
	float p = camMode == CAM_RTS ? (benchRtsFixed ? RTS_PITCH : rtsPitch) : camPitch;
	float dist = camMode == CAM_RTS ? rtsDist : tppDist;
	float tz = camMode == CAM_RTS ? RTS_TARGET_Z : 105.0f;
	fwd[0] = cosf(p) * sinf(rtsYaw); fwd[1] = cosf(p) * cosf(rtsYaw); fwd[2] = sinf(p);
	eye[0] = chr[0] - fwd[0] * dist;
	eye[1] = chr[1] - fwd[1] * dist;
	eye[2] = chr[2] + tz - fwd[2] * dist;
	// never inside a hill behind the character (hardware test 6, shot 2): up over the ground
	float clear = camMode == CAM_RTS ? 80.0f : 25.0f;
	if (groundAt(eye[0], eye[1], &g) && eye[2] < g + clear) {
		eye[2] = g + clear;
		if (camMode == CAM_TPP) {   // look at the head from there
			float vx = chr[0] - eye[0], vy = chr[1] - eye[1], vz = chr[2] + tz - eye[2];
			float l = sqrtf(vx * vx + vy * vy + vz * vz);
			if (l > 1) { fwd[0] = vx / l; fwd[1] = vy / l; fwd[2] = vz / l; }
			p = asinf(fwd[2]);
		}
	}
	yaw = rtsYaw; pitch = p;
}

// The draw distance and what is loaded follow the camera: RTS sees ~1800 round the character,
// TPP / FPP to the horizon (the fog hides the end of TPP_FAR); under the water ~1500.
static void setCamLimits(void)
{
	if (!rtsMode) return;
	if (underwater) { farDist = 1600.0f; fogStart = 0.0f; }
	else if (camMode == CAM_RTS) { farDist = RTS_FAR; fogStart = 0.7f; }
	else { farDist = TPP_FAR; fogStart = 0.45f; }
	loadDist = camMode == CAM_RTS ? RTS_LOAD : TPP_FAR + 500.0f;
	dropDist = loadDist + 3000.0f;
}

static void setRts(bool on)
{
	rtsMode = on;
	if (on) {
		autoFar = false;
		setCamLimits();
		rtsYaw = yaw;
		if (!chrPlaced) { chr[0] = eye[0]; chr[1] = eye[1]; chr[2] = eye[2] - 200.0f; }
	} else {
		autoFar = true; fogStart = 0.45f;
		pitch = -0.3f;
		underwater = false;
	}
}

static float maxFar(void) { return radius == 0 ? 6000.0f : radius * CELL + 3000.0f; }

// The draw distance from the GPU time, averaged over 20 frames, with a dead band: down 8 %
// above the budget, up 4 % well under it; a frame far over it cuts 15 % at once.
static void pace(float gpuMs)
{
	if (!autoFar) return;
	if (paceCooldown > 0) paceCooldown--;
	if (gpuMs > gpuBudget * 1.6f && paceCooldown == 0) {
		farDist *= 0.85f;
		paceCooldown = 10;
		paceGpu = 0; paceFrames = 0;
	} else {
		paceGpu += gpuMs;
		if (++paceFrames == 20) {
			float avg = paceGpu / paceFrames;
			if (avg > gpuBudget * 1.05f) farDist *= 0.92f;
			else if (avg < gpuBudget * 0.85f) farDist *= 1.04f;
			paceGpu = 0; paceFrames = 0;
		}
	}
	if (farDist < 2500.0f) farDist = 2500.0f;
	if (farDist > maxFar()) farDist = maxFar();
}

static void setCap(bool on)
{
	cap30 = on;
	C3D_FrameRate(on ? 30.0f : 60.0f);
}

// ------------------------------------------------------------------ screenshot

// The top screen as it was shown last, into a 400x240 BMP (the framebuffer is BGR, 240
// pixels per column, bottom to top).
static void saveShotOf(gfxScreen_t scr, const char* path)
{
	u16 w, h;
	u8* fb = gfxGetFramebuffer(scr, GFX_LEFT, &w, &h);
	if (gfxGetScreenFormat(scr) != GSP_BGR8_OES) return;
	FILE* f = fopen(path, "wb");
	if (!f || !fb) { if (f) fclose(f); return; }
	int W = h, H = w;   // 400 x 240
	u32 row = W * 3, size = 54 + row * H;
	u8 hd[54] = { 'B', 'M' };
	memcpy(hd + 2, &size, 4);
	u32 v = 54; memcpy(hd + 10, &v, 4);
	v = 40; memcpy(hd + 14, &v, 4);
	memcpy(hd + 18, &W, 4); memcpy(hd + 22, &H, 4);
	hd[26] = 1; hd[28] = 24;
	fwrite(hd, 1, 54, f);
	u8* line = malloc(row);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) memcpy(line + x * 3, fb + (x * H + y) * 3, 3);
		fwrite(line, 1, row, f);
	}
	free(line);
	fclose(f);
}

static void saveShot(const char* path) { saveShotOf(GFX_TOP, path); }

// ------------------------------------------------------------------ benchmark

// sdmc:/3ds/mwcell3d/bench.txt present: the benchmark, logged to bench.log, then the app quits
// (and bench.txt becomes bench.done, so the next start is the manual mode).
//   phase 0, 1: radius 0 and 1 at a fixed distance, 60 fps cap, standing at the start and
//               looking in 8 directions (the hardware test 1 numbers, to compare)
//   phase 2:    the Old 3DS profile (radius 1, auto distance, 30 fps) walking the route
//               Seyda Neen -> Pelagiad at 400 units/s, looking left and right; halfway it
//               turns back for 4 s (across a cell border) and goes on
//   phase 3:    the same profile sprinting the route back at 1000 units/s
//   phase 4:    the RTS camera: standing 12 s while the camera goes once round the character,
//               then the character running the route at 420 units/s, the camera swaying round it
// bench.txt saying "rts": only phase 4.
enum { B_OFF, B_STATIC, B_WALK, B_RTS, B_DONE, B_VIEWS };
static int bPhase = B_OFF, bAngle = -1, bFrame;
static float bCpu, bCpuMax, bGpu, bDraws, bTris, bFrameMs;
static float startEye[3], startYaw;
static bool benchOnlyRts, benchViews;
static int viewsFrom;   // bench.txt "views 12": from that step
static int benchFx;   // the looks of the RTS test: bench.txt "rts nocaustics" -> 1

// walk state
static float wDist, wSpeed, wT, wTurnT;
static int wDir, wStage;      // wStage: 2 walk there, 3 sprint back
static int wTurn;          // 0 not yet, 1 going back, 2 done
// per second
static float sT, sFrameMs, sFrameMax, sCpu, sGpu, sTris, sDraws, sFar;
static int sFrames, sMissingMax;
// per walk
static int wFrames, wHitch, wOver34, wLoads, wSkip;
static float wWorst, wFrameSum, wSetupMax;

static float routeLen(void) { return (nroute - 1) * 256.0f; }

static void routeAt(float d, float out[3], float* heading)
{
	if (nroute < 2) { memcpy(out, eye, 12); *heading = 0; return; }
	float s = d / 256.0f;
	if (s < 0) s = 0;
	if (s > nroute - 1.001f) s = nroute - 1.001f;
	int i = (int)s;
	float f = s - i;
	for (int k = 0; k < 3; k++) out[k] = route[i][k] * (1 - f) + route[i + 1][k] * f;
	*heading = atan2f(route[i + 1][0] - route[i][0], route[i + 1][1] - route[i][1]);
}

static void benchResetStats(void)
{
	wT = 0; wTurnT = 0; wTurn = 0;
	wFrames = wHitch = wOver34 = wLoads = 0;
	wSkip = 3;   // the frames after the log was written to the SD (logFlush) are not the streaming's
	wWorst = wFrameSum = wSetupMax = 0;
	sT = sFrameMs = sFrameMax = sCpu = sGpu = sTris = sDraws = sFar = 0;
	sFrames = sMissingMax = 0;
}

static int rtsShot;          // the next RTS screenshot (0: standing, 1-4: along the route)
static bool rtsWaiting;      // waiting for the cells round the start

static void benchStartRts(void)
{
	benchRtsFixed = true;
	bPhase = B_RTS;
	wStage = 4;
	benchResetStats();
	wDist = 0;
	rtsShot = 0;
	rtsWaiting = true;
	fxMode = benchFx;
	radius = 1; setCap(true);
	yaw = startYaw;
	chrPlaced = false;
	memcpy(eye, startEye, 12);
	setRts(true);
	chr[0] = startEye[0]; chr[1] = startEye[1]; chr[2] = startEye[2] - 200.0f;
	logw("rts camera (pitch %.0f deg, distance %.0f, far %.0f), radius 1, 30 fps cap: 12 s standing (the camera once round), then running the route at %.0f units/s:\n",
	     RTS_PITCH * 180.0f / (float)M_PI, RTS_DIST, RTS_FAR, RUN_SPEED);
}

static void benchStartWalk(int stage)
{
	wStage = stage;
	wT = 0; wTurnT = 0; wTurn = 0;
	wSpeed = stage == 2 ? 400.0f : 1000.0f;
	wDir = stage == 2 ? 1 : -1;
	wDist = stage == 2 ? 0 : routeLen();
	wFrames = wHitch = wOver34 = wLoads = 0;
	wSkip = 3;   // the frames after the log was written to the SD (logFlush) are not the streaming's
	wWorst = wFrameSum = wSetupMax = 0;
	sT = sFrameMs = sFrameMax = sCpu = sGpu = sTris = sDraws = sFar = 0;
	sFrames = sMissingMax = 0;
	radius = 1; autoFar = true; farDist = 8000.0f; setCap(true);
	logw("%s, radius 1, auto distance (GPU budget %.0f ms), 30 fps cap, %.0f units/s:\n",
	     stage == 2 ? "walk Seyda Neen -> Pelagiad (turning back halfway)" : "sprint Pelagiad -> Seyda Neen",
	     gpuBudget, wSpeed);
}

// bench.txt "views": the character's cameras, the torch, a floor, the sea and a bridge, each a
// screenshot (view_<n>.bmp) for looking at on the PC. Each step: where to put the character (x == 0:
// where it is), the camera, the torch, the camera's turn, the circle pad (forward), run, dive, seconds.
typedef struct { const char* name; float x, y, z; int cam; bool torch; float yaw, pad; bool run, dive; float secs, dist; int act; } View;
enum { V_NONE, V_NPC10, V_NPC20, V_ATTACK, V_JUMP, V_NPC_CLEAR, V_FIGHT, V_FIGHT2, V_USE1, V_USE2 };
static const View VIEWS[] = {
	{ "start rts",       1, 0, 0, CAM_RTS, false, -1, 0, false, false, 2.0f },
	{ "start tpp",       0, 0, 0, CAM_TPP, false, -1, 0, false, false, 1.5f },
	{ "start fpp",       0, 0, 0, CAM_FPP, false, -1, 0, false, false, 1.5f },
	{ "torch tpp close", 0, 0, 0, CAM_TPP, true, -1, 0, false, false, 1.5f, 120 },
	{ "platform tpp",    -12127, -74545, 900, CAM_TPP, false, 0.0f, 0, false, false, 2.5f, 230 },
	{ "to the sea",      -14208, -76288, 400, CAM_TPP, false, 4.71f, 1, true, false, 2.5f },
	{ "swimming",        0, 0, 0, CAM_TPP, false, 4.71f, 1, false, false, 2.5f },
	{ "diving tpp",      0, 0, 0, CAM_TPP, false, 4.71f, 0.4f, false, true, 3.0f },
	{ "diving fpp",      0, 0, 0, CAM_FPP, false, 4.71f, 0, false, true, 1.5f },
	{ "bridge tpp",      -18144, -19840, 900, CAM_TPP, false, 0.0f, 0, false, false, 3.0f },
	{ "bridge rts torch", 0, 0, 0, CAM_RTS, true, 0.0f, 0, false, false, 2.0f },
	{ "bridge walk",     0, 0, 0, CAM_TPP, false, 1.57f, 1, false, false, 3.0f },
	{ "10 npcs tpp",     1, 0, 0, CAM_TPP, false, -1, 0, false, false, 3.0f, 420, V_NPC10 },
	{ "30 npcs rts",     0, 0, 0, CAM_RTS, false, -1, 0, false, false, 3.0f, 0, V_NPC20 },
	{ "sword attack",    0, 0, 0, CAM_TPP, false, -1, 0, false, false, 0.7f, 200, V_ATTACK },
	{ "jump",            0, 0, 0, CAM_TPP, true, -1, 0, false, false, 0.22f, 0, V_JUMP },
	{ "fight: chop",     0, 0, 0, CAM_TPP, false, -1, 0, false, false, 1.0f, 260, V_FIGHT },
	{ "fight: its turn", 0, 0, 0, CAM_TPP, false, -1, 0, false, false, 2.6f, 0, V_FIGHT2 },
	// the objects (objects.bin) and their scripts: a door near the start used once (the script: knocking),
	// then twice (open, swinging); a barrel searched (the script counts)
	{ "door: knock",     -10468, -70688, 400, CAM_RTS, false, 2.83f, 0, false, false, 1.0f, 0, V_USE1 },
	{ "door: enter",     0, 0, 0, CAM_RTS, false, 2.83f, 0, false, false, 2.0f, 0, V_USE1 },
	{ "inside rts",      0, 0, 0, CAM_RTS, false, -1, 0, false, false, 1.5f, 0 },
	{ "inside tpp",      0, 0, 0, CAM_TPP, false, -1, 0, false, false, 1.5f, 300 },
	{ "door: exit",      0, 0, 0, CAM_TPP, false, -1, 0, false, false, 2.0f, 300, V_USE1 },
	{ "barrel: search",  -10781, -71763, 300, CAM_RTS, false, 1.57f, 0, false, false, 1.0f, 0, V_USE2 },
};
#define NVIEWS (int)(sizeof VIEWS / sizeof VIEWS[0])
static int vStep;
static float vT;
static bool vWait, vJump, vFight;
static int vSpawn, vUse;

static void viewsBegin(int k)
{
	const View* v = &VIEWS[k];
	vStep = k; vT = 0; vWait = true;
	if (v->x == 1) { chr[0] = startEye[0]; chr[1] = startEye[1]; chr[2] = startEye[2]; chrPlaced = false; }
	else if (v->x != 0) { chr[0] = v->x; chr[1] = v->y; chr[2] = v->z; chrPlaced = false; swimming = false; chrVz = 0; }
	camMode = v->cam;
	camPitch = camMode == CAM_TPP ? -0.22f : 0;
	torchOn = v->torch;
	if (v->dist > 0) tppDist = v->dist;
	if (v->act == V_USE1 || v->act == V_USE2) { rtsDist = 900.0f; rtsPitch = -1.25f; }   // (the objects' views: from high up, over the roofs)
	if (v->act == V_NPC10) { clearNpcs(); vSpawn = 10; }
	if (v->act == V_NPC20) vSpawn = 20;
	if (v->act == V_ATTACK && chOk) { PL->weapon = 0; PL->dirty = true; PL->act = A_CHOP1H; }
	if (v->act == V_JUMP && chOk) { PL->weapon = -1; PL->dirty = true; vJump = true; }
	if (v->act == V_FIGHT && chOk) { PL->weapon = 0; PL->dirty = true; clearNpcs(); vFight = true; }
	vUse = v->act == V_USE1 ? 1 : v->act == V_USE2 ? 2 : 0;
	if (v->yaw >= 0) rtsYaw = v->yaw; else if (v->x == 0 && inInterior >= 0) rtsYaw = chrYaw; else rtsYaw = startYaw;
	setCamLimits();
}

static void benchStartViews(void)
{
	bPhase = B_VIEWS;
	radius = 1; setCap(true);
	fxMode = 0;
	yaw = startYaw;
	setRts(true);
	logw("views (bench.txt views): the character's cameras, torch, floor, sea, bridge\n");
	if (viewsFrom > 0 && viewsFrom < NVIEWS) {
		viewsBegin(viewsFrom);
		if (VIEWS[viewsFrom].act == V_NPC20) vSpawn = 30;   // (the 10 of the step before too)
	} else viewsBegin(0);
}

// Moves the camera along the route (before the frame).
static void benchCamera(float dt, float fwd[3])
{
	if (bPhase == B_VIEWS) {
		const View* v = &VIEWS[vStep];
		if (vWait) {   // the cells round the character first
			chrUpdate(dt, 0, 0, false, false, false);
			if (chrPlaced && job.state == JOB_IDLE && !cellsMissing) vWait = false;
		} else {
			if (vSpawn) { spawnNpcs(vSpawn); vSpawn = 0; }
			if (vFight && !fight.on) fightStart();   // (once the player stands: the enemy at its height)
			if (vFight && fight.on && vT > 0.3f) { fightAttack(2); vFight = false; }
			vT += dt;
			if (vUse && vT > 0.3f + (v->act == V_USE2 && vUse == 1 ? 0.3f : 0)) {   // A on the nearest object
				{   // facing the nearest object (the camera looks from the side)
					float bd = 1e9f;
					for (int q = 0; q < nobj; q++) {
						float dx = objs[q].pos[0] - chr[0], dy = objs[q].pos[1] - chr[1];
						if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; chrYaw = atan2f(dx, dy); }
					}
				}
				int o = objNearest(chr, chrYaw, 250.0f);
				logw("    view use: #%d %s\n", o, o >= 0 ? objId(o) : "-");
				objActivate(o);
				vUse--;
			}
			chrUpdate(dt, 0, v->pad, v->run, v->dive, vJump);
			vJump = false;
		}
		rtsCamera(fwd);
		return;
	}
	if (bPhase == B_STATIC) {
		memcpy(eye, startEye, 12);
		pitch = -0.12f;
		yaw = startYaw + (bAngle < 0 ? 0 : bAngle) * (float)M_PI / 4;
	} else if (bPhase == B_WALK) {
		wT += dt;
		int dir = wDir;
		if (wStage == 2) {   // halfway: 4 s back the way it came, then on again
			if (wTurn == 0 && wDist >= routeLen() / 2) wTurn = 1;
			if (wTurn == 1) { dir = -1; wTurnT += dt; if (wTurnT > 4.0f) wTurn = 2; }
		}
		wDist += dir * wSpeed * dt;
		float heading;
		routeAt(wDist, eye, &heading);
		if (dir < 0) heading += (float)M_PI;
		yaw = heading + 0.9f * sinf(wT * 0.6f);   // looking round while walking
		pitch = -0.06f;
	} else if (bPhase == B_RTS) {
		float g;
		if (rtsWaiting) {   // the cells round the start first, then the clock
			if (!chrPlaced && groundAt(chr[0], chr[1], &g)) { chr[2] = g > -10000.0f ? g : 0; chrPlaced = true; }
			if (chrPlaced && job.state == JOB_IDLE && !cellsMissing) { rtsWaiting = false; benchResetStats(); }
			rtsYaw = startYaw;
		} else {
			wT += dt;
			if (wT < 12.0f) {
				rtsYaw = startYaw + wT * 2.0f * (float)M_PI / 12.0f;
				chrSwing = 0;
				PL->speed = 0;
				if (chOk) { PL->outfit = chNOutfit - 1; PL->dirty = true; }   // the standing screenshot: the last outfit
			} else {
				float before[2] = { chr[0], chr[1] }, p[3], heading;
				wDist += RUN_SPEED * dt;
				routeAt(wDist, p, &heading);
				chr[0] = p[0]; chr[1] = p[1];
				if (groundAt(chr[0], chr[1], &g) && g > -10000.0f) chr[2] = g;
				chrYaw = heading;
				float moved = sqrtf((chr[0] - before[0]) * (chr[0] - before[0]) + (chr[1] - before[1]) * (chr[1] - before[1]));
				chrPhase += moved * (float)M_PI / 110.0f;
				chrSwing = 1;
				PL->speed = dt > 0 ? moved / dt : 0;
				PL->run = true;
				if (chOk) { int o = (1 + (int)(wDist / (routeLen() / 4))) % chNOutfit; if (o != PL->outfit) { PL->outfit = o; PL->dirty = true; } }   // each quarter another
				rtsYaw = heading + 0.8f * sinf((wT - 12.0f) * 0.5f);
			}
		}
		rtsCamera(fwd);
		return;
	}
	fwd[0] = cosf(pitch) * sinf(yaw); fwd[1] = cosf(pitch) * cosf(yaw); fwd[2] = sinf(pitch);
}

static void benchAfterFrame(float rawMs, float cpuMs, float gpuMs, float setupMs, bool* quit)
{
	if (bPhase == B_STATIC) {
		bFrame++;
		if (bAngle < 0) {   // waiting for the cells
			if (job.state != JOB_IDLE || cellsMissing) bFrame = 0;
			if (bFrame > 20) {
				int loaded = 0; u32 lt = 0, lb = 0;
				for (int i = 0; i < MAX_CELLS; i++) if (cells[i].used) { loaded++; lt += cells[i].tris; lb += cells[i].size; }
				logw("radius %d, far %.0f, 60 fps cap: %d cells, %lu tris, %lu KB, linear free %lu KB\n",
				     radius, farDist, loaded, lt, lb / 1024, linearSpaceFree() / 1024);
				bAngle = 0; bFrame = 0;
			}
			return;
		}
		if (bFrame > 10) {
			bFrameMs += rawMs; bCpu += cpuMs; if (cpuMs > bCpuMax) bCpuMax = cpuMs;
			bGpu += gpuMs; bDraws += statDraws; bTris += statTris;
		}
		if (bFrame == 40) {
			float n = 30;
			logw("  dir %d: fps %.1f, cpu %.2f ms (max %.2f), gpu %.2f ms, draws %.0f, tris %.0f\n",
			     bAngle, 1000.0f * n / bFrameMs, bCpu / n, bCpuMax, bGpu / n, bDraws / n, bTris / n);
			if (bAngle == 0) {
				char p[64];
				snprintf(p, sizeof p, "sdmc:/3ds/mwcell3d/r%d_d0.bmp", radius);
				saveShot(p);
			}
			bCpu = bCpuMax = bGpu = bDraws = bTris = bFrameMs = 0;
			bFrame = 0;
			if (++bAngle == 8) {
				bAngle = -1;
				logFlush();
				if (++radius == 2) { bPhase = B_WALK; benchStartWalk(2); }
				else farDist = 12000.0f;
			}
		}
		return;
	}
	if (bPhase == B_VIEWS) {
		if (++bFrame % 30 == 0)
			logw("    (step %d %s, t %.1f, npcs %d, frame %.1f ms, cpu %.2f gpu %.2f pose %.2f draw %.2f)\n", vStep,
			     vWait ? "waiting" : "running", vT, nNpc, rawMs, cpuMs, gpuMs, chAnimMs, chDrawMs);
		if (bFrame % 30 == 0) logFlush();
		if (vWait || vT < VIEWS[vStep].secs) return;
		char p[64];
		snprintf(p, sizeof p, "sdmc:/3ds/mwcell3d/view_%d.bmp", vStep);
		saveShot(p);
		snprintf(p, sizeof p, "sdmc:/3ds/mwcell3d/view_%d_b.bmp", vStep);
		saveShotOf(GFX_BOTTOM, p);
		float g = 0;
		groundAt(chr[0], chr[1], &g);
		logw("  view %d %-16s: anim %s speed %.0f, chr %.0f %.0f %.0f ground %.0f floor %.0f%s%s%s | npcs %d pose %.2f draw %.2f | eye z %.0f | fps-ish frame %.1f ms gpu %.2f cpu %.2f far %.0f | obj draws %d\n",
		     vStep, VIEWS[vStep].name, chOk && PL->cur >= 0 ? chAnim[PL->cur].name : "-", PL->speed, chr[0], chr[1], chr[2], g, floorAt(chr[0], chr[1], chr[2], 45.0f),
		     swimming ? " swimming" : "", underwater ? " underwater" : "", torchOn ? " torch" : "", nNpc, chAnimMs, chDrawMs, eye[2], rawMs, gpuMs, cpuMs, farDist, statObjDraws);
		logFlush();
		if (vStep + 1 < NVIEWS) viewsBegin(vStep + 1);
		else { bPhase = B_DONE; *quit = true; }
		return;
	}
	if (bPhase != B_WALK && bPhase != B_RTS) return;
	if (bPhase == B_RTS) {
		if (rtsWaiting) return;
		// screenshots: standing (6 s), then at each quarter of the route
		bool shot = false;
		if (rtsShot == 0 && wT >= 6.0f) shot = true;
		else if (rtsShot >= 1 && rtsShot <= 4 && wDist >= routeLen() * (rtsShot - 0.5f) / 4.0f) shot = true;
		if (shot) {
			char p[64];
			snprintf(p, sizeof p, "sdmc:/3ds/mwcell3d/rts_%d.bmp", rtsShot);
			saveShot(p);
			rtsShot++;
			wSkip = 3;
		}
		if (wT >= 12.0f && wT - rawMs / 1000.0f < 12.0f) {
			// the standing part's last second may be partial: log it as it is
			logw("  -- running\n");
		}
	}
	// the walk: per second and per walk
	if (wSkip > 0) { wSkip--; return; }
	wFrames++; wFrameSum += rawMs;
	if (rawMs > wWorst) wWorst = rawMs;
	if (rawMs > 34.5f) wOver34++;       // longer than a 30 fps frame: a missed frame
	if (rawMs > 50.0f) wHitch++;        // a visible stop
	if (setupMs > 0) { wLoads++; if (setupMs > wSetupMax) wSetupMax = setupMs; }
	sFrames++; sFrameMs += rawMs; if (rawMs > sFrameMax) sFrameMax = rawMs;
	sCpu += cpuMs; sGpu += gpuMs; sTris += statTris; sDraws += statDraws; sFar += farDist;
	if (cellsMissing > sMissingMax) sMissingMax = cellsMissing;
	sT += rawMs / 1000.0f;
	if (sT >= 1.0f) {
		int loaded = 0;
		for (int i = 0; i < MAX_CELLS; i++) if (cells[i].used) loaded++;
		float n = sFrames;
		logw("  t=%4.1fs cell %d,%d: fps %.1f, frame %.1f ms (max %.1f), cpu %.2f, gpu %.2f, far %.0f, draws %.0f, tris %.0f, cells %d, missing %d, free %lu KB\n",
		     wT, (int)floorf(eye[0] / CELL), (int)floorf(eye[1] / CELL), n / sT, sFrameMs / n, sFrameMax, sCpu / n,
		     sGpu / n, sFar / n, sDraws / n, sTris / n, loaded, sMissingMax, linearSpaceFree() / 1024);
		sT = sFrameMs = sFrameMax = sCpu = sGpu = sTris = sDraws = sFar = 0;
		sFrames = sMissingMax = 0;
	}
	bool end = wStage == 2 || wStage == 4 ? wDist >= routeLen() : wDist <= 0;
	if (end) {
		logw("  => %d frames, avg fps %.1f, missed frames (>34.5 ms) %d, stops (>50 ms) %d, worst %.1f ms, loads %d (setup max %.1f ms), load failures %d\n",
		     wFrames, 1000.0f * wFrames / wFrameSum, wOver34, wHitch, wWorst, wLoads, wSetupMax, loadFailed);
		logFlush();
		if (wStage == 2) benchStartWalk(3);
		else if (wStage == 3) benchStartRts();
		else { bPhase = B_DONE; *quit = true; }
	}
}

// ------------------------------------------------------------------ sound

// The player's sounds from what it does: a footstep at each step of the walk cycle (left and right
// in turn; light, medium or heavy by the clothes), strokes swimming, a splash going in, landing
// after a fall; the torch's crackle and the sea's hum under the water as loops; the music lower
// under the water.
static void soundUpdate(void)
{
	static int lastStep;
	static bool wasAir, wasSwim;
	int step = (int)floorf(chrPhase / (float)M_PI);
	if (step != lastStep && chrSwing > 0.3f) {
		bool left = step & 1;
		if (swimming) audioPlay(left ? SND_SWIML : SND_SWIMR, 0.5f, left ? -0.3f : 0.3f);
		else if (!chrAir) {
			int w = 0;   // the weight by the outfit's name
			for (int k = 0; chOk && k < invItems(); k++) {
				if (!PL->worn[k]) continue;
				const char* n = chItem[k].name;
				if (strstr(n, "iron") || strstr(n, "glass")) w = 2;
				else if (strstr(n, "bonemold") && w < 1) w = 1;
			}
			int base = w == 0 ? SND_WALKL_LT : w == 1 ? SND_WALKL_MD : SND_WALKL_HV;
			audioPlay(base + (PL->run ? 2 : 0) + (left ? 0 : 1), 0.55f, left ? -0.25f : 0.25f);
		}
	}
	lastStep = step;
	if (wasAir && !chrAir && !swimming) audioPlay(SND_LAND, 0.7f, 0);
	if (!wasSwim && swimming) audioPlay(SND_SPLASH, 0.8f, 0);
	wasAir = chrAir; wasSwim = swimming;
	audioLoop(6, SND_TORCH, torchOn && !swimming ? 0.35f : 0);
	audioLoop(7, SND_UNDERWATER, underwater ? 0.7f : 0);
	audioMusicVolume(underwater ? 0.15f : 0.55f);
}

// ------------------------------------------------------------------ main

int main(void)
{
	osSetSpeedupEnable(true);
	gfxInitDefault();
	// the bottom screen: the text console while benchmarking, else the inventory window (ui.c)
	{
		FILE* bf = fopen("sdmc:/3ds/mwcell3d/bench.txt", "rb");
		useConsole = bf != NULL;
		if (bf) {   // ("views" shows the window: its screenshots have the bottom screen too)
			char what[16] = { 0 };
			fread(what, 1, sizeof what - 1, bf);
			if (strstr(what, "views")) useConsole = false;
			fclose(bf);
		}
	}
	if (useConsole) consoleInit(GFX_BOTTOM, NULL);
	romfsInit();
	dataInit();   // the baked data: from the SD card when it is there (the app without romfs), else romfs
	bool sound = audioInit();
	C3D_Init(0x180000);   // 1.5 MB: each skinned mesh sends its bone palette (~14 KB per character)
	if (!useConsole) { uiInit(); uiOn = true; }

	gfxSet3D(true);
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTarget* topR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (topR) C3D_RenderTargetSetOutput(topR, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	// anti-aliasing: 2 x 2 the pixels, scaled down by the display transfer (4 x the fill)
	// (anti-aliasing, 2 x 2 the pixels: 9 ms more GPU for little to see, hardware test 7: off)
	C3D_RenderTarget* topAA = NULL;
	C3D_RenderTarget* curLeft = top;
	const u32 AA_FLAGS = (DISPLAY_TRANSFER_FLAGS & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY);

	dvlb = DVLB_ParseFile((u32*)world_shbin, world_shbin_size);
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	uMvp = shaderInstanceGetUniformLocation(prog.vertexShader, "mvp");
	uOrigin = shaderInstanceGetUniformLocation(prog.vertexShader, "origin");
	uFog = shaderInstanceGetUniformLocation(prog.vertexShader, "fogRow");
	uDetail = shaderInstanceGetUniformLocation(prog.vertexShader, "detail");
	uTorch = shaderInstanceGetUniformLocation(prog.vertexShader, "torch");
	uTorchCol = shaderInstanceGetUniformLocation(prog.vertexShader, "torchCol");
	AttrInfo_Init(&attr);
	AttrInfo_AddLoader(&attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(&attr, 1, GPU_SHORT, 2);
	AttrInfo_AddLoader(&attr, 2, GPU_UNSIGNED_BYTE, 4);
	wDvlb = DVLB_ParseFile((u32*)water_shbin, water_shbin_size);
	shaderProgramInit(&wProg);
	shaderProgramSetVsh(&wProg, &wDvlb->DVLE[0]);
	wMvp = shaderInstanceGetUniformLocation(wProg.vertexShader, "mvp");
	wOrigin = shaderInstanceGetUniformLocation(wProg.vertexShader, "origin");
	wFog = shaderInstanceGetUniformLocation(wProg.vertexShader, "fogRow");
	wUvA = shaderInstanceGetUniformLocation(wProg.vertexShader, "uvA");
	wUvB = shaderInstanceGetUniformLocation(wProg.vertexShader, "uvB");
	wFres = shaderInstanceGetUniformLocation(wProg.vertexShader, "fres");
	wRefl = shaderInstanceGetUniformLocation(wProg.vertexShader, "refl");
	makeRipples();
	makeDetail();
	shV = (Vtx*)linearAlloc((SH_SEG + 1) * sizeof(Vtx));
	shI = (u16*)linearAlloc(SH_SEG * 3 * sizeof(u16));
	if (shV && shI) {
		for (int i = 0; i < SH_SEG; i++) { shI[i * 3] = 0; shI[i * 3 + 1] = 1 + i; shI[i * 3 + 2] = 1 + (i + 1) % SH_SEG; }
		GSPGPU_FlushDataCache(shI, SH_SEG * 3 * sizeof(u16));
		BufInfo_Init(&shBuf);
		BufInfo_Add(&shBuf, shV, sizeof(Vtx), 3, 0x210);
	}
	flV = (Vtx*)linearAlloc(2 * (FL_SEG + 1) * sizeof(Vtx));
	flI = (u16*)linearAlloc(2 * FL_SEG * 3 * sizeof(u16));
	if (flV && flI) {
		for (int f = 0; f < 2; f++)
			for (int i = 0; i < FL_SEG; i++) {
				u16 o = f * (FL_SEG + 1);
				flI[(f * FL_SEG + i) * 3] = o; flI[(f * FL_SEG + i) * 3 + 1] = o + 1 + i; flI[(f * FL_SEG + i) * 3 + 2] = o + 1 + (i + 1) % FL_SEG;
			}
		GSPGPU_FlushDataCache(flI, 2 * FL_SEG * 3 * sizeof(u16));
		BufInfo_Init(&flBuf);
		BufInfo_Add(&flBuf, flV, sizeof(Vtx), 3, 0x210);
	}
	chrV = (Vtx*)linearAlloc(CHR_MAXV * sizeof(Vtx));
	chrI = (u16*)linearAlloc(CHR_MAXI * sizeof(u16));
	BufInfo_Init(&chrBuf);
	BufInfo_Add(&chrBuf, chrV, sizeof(Vtx), 3, 0x210);

	printf("3dsOpenworld - Morrowind cell test\n");
	if (!sound) printf("\x1b[33mno sound: run DSP1 once (it makes\nsdmc:/3ds/dspfirm.cdc)\x1b[0m\n");
	u64 t0 = ticks();
	if (!loadIndex() || !loadTextures()) {
		printf("\x1b[31mromfs data missing (tools/mwbake.py)\x1b[0m\nSTART to quit\n");
		while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
		goto done;
	}
	float texMs = msSince(t0);
	{
		u64 c0 = ticks();
		bool ok = loadCharacter();
		printf("character: %s (%d meshes, %d outfits) %.0f ms\n", ok ? "ok" : "none", chNMesh, chNOutfit, msSince(c0));
	}
	objsOk = objLoad();
	pgLoad();
	if (objsOk) objInitGpu();
	loadInteriors();
	printf("%d textures %lu KB in %.0f ms\n", ntex, texBytes / 1024, texMs);

	// the loader thread: above the main thread on its core (it mostly waits on the SD)
	LightEvent_Init(&jobEvent, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	loaderThread = threadCreate(loaderMain, NULL, 32 * 1024, prio - 1, -2, false);

	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/mwcell3d", 0777);
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	{
		FILE* bf = fopen("sdmc:/3ds/mwcell3d/bench.txt", "rb");
		if (bf) {
			char what[16] = { 0 };
			fread(what, 1, sizeof what - 1, bf);
			fclose(bf);
			benchOnlyRts = strstr(what, "rts") != NULL;
			benchViews = strstr(what, "views") != NULL;
			if (benchViews) viewsFrom = atoi(strstr(what, "views") + 5);
			waterTint = strstr(what, "blue") ? 1 : 0;
			benchFx = strstr(what, "calm") ? FX_CALM : strstr(what, "nocaus") ? 1 : 0;
			logFile = fopen("sdmc:/3ds/mwcell3d/bench.log", "w");
			bPhase = B_STATIC;
			rtsMode = false;
			fxMode = FX_PLAIN;   // the tests T1-T5 had the plain water
			radius = 0; farDist = 7000.0f; autoFar = false; setCap(false);
		} else {
			logFile = fopen("sdmc:/3ds/mwcell3d/play.log", "a");
			setCap(true);
			setRts(true);
		}
		logw("--- %s 3DS, heap %lu KB, linear %lu KB, textures %d (%lu KB) in %.0f ms, %d cells baked, route %d points%s\n",
		     n3ds ? "New" : "Old", __ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, ntex, texBytes / 1024, texMs,
		     navail, nroute, loaderThread ? "" : ", NO LOADER THREAD");
		logw("    sound: %s, data from %s\n", sound ? "on" : "OFF (no sdmc:/3ds/dspfirm.cdc: run DSP1.3dsx once)", dataDir);
		logw("    objects: %s, %d (%d models, %d parts, %lu KB), interiors %d\n", objsOk ? "ok" : "NONE", nobj, objNModels, objNParts, objBytes / 1024, ninterior);
		scriptsOk = scriptInit();
		if (!scriptsOk) { logw("    scripts: %s\n", scriptError()); if (scriptError()[0]) gameMessage(scriptError()); }
		logFlush();
		if (bPhase != B_OFF) printf("\x1b[33mBENCHMARK running (a few min),\nthis screen stays still meanwhile\x1b[0m\n");
	}
	startEye[0] = eye[0]; startEye[1] = eye[1]; startEye[2] = eye[2];
	startYaw = yaw;
	if (benchViews) benchStartViews();
	else if (benchOnlyRts) benchStartRts();

	u64 last = ticks(), fpsT = last;
	int frames = 0, playTick = 0;
	float fps = 0, cpuMs = 0, cpuAcc = 0, cpuMax = 0, cpuMaxShown = 0;
	bool quit = false;
	float prevEye[2] = { eye[0], eye[1] };
	while (aptMainLoop() && !quit) {
		u64 now = ticks();
		float rawMs = (now - last) / (float)CPU_TICKS_PER_MSEC;   // the real frame time, stops included
		float dt = rawMs / 1000.0f;
		last = now;
		if (dt > 0.1f) dt = 0.1f;

		hidScanInput();
		bool bench = bPhase != B_OFF;
		u32 down = bench ? 0 : hidKeysDown(), held = bench ? 0 : hidKeysHeld();
		if (down & KEY_START) break;
		if ((down & KEY_SELECT) && (!rtsMode || (held & KEY_L))) setRts(!rtsMode);
		else if ((down & KEY_SELECT) && camMode != CAM_FPP) { curOn = !curOn; audioPlay(SND_CLICK, 0.5f, 0); }
		if ((down & KEY_TOUCH) && uiOn && chOk) {
			touchPosition tp;
			hidTouchRead(&tp);
			int hit = uiTouch(&uiState, tp.px, tp.py);
			if (hit >= UI_ITEM && hit < UI_ITEM + UI_MAXITEMS) { uiSel = uiState.page * UI_MAXITEMS + hit - UI_ITEM; invApply(uiSel); }
			else if (hit == UI_PREV) { uiState.page--; audioPlay(SND_CLICK, 0.5f, 0); }
			else if (hit == UI_NEXT) { uiState.page++; audioPlay(SND_CLICK, 0.5f, 0); }
			else if (hit == UI_CAMERA) {
				if (!rtsMode) setRts(true);
				else { camMode = (camMode + 1) % NCAM; camPitch = camMode == CAM_TPP ? -0.22f : 0; setCamLimits(); }
				audioPlay(SND_CLICK, 0.6f, 0);
			} else if (hit == UI_TORCH) { torchOn = !torchOn; audioPlay(torchOn ? SND_CLICK : SND_TORCHOUT, 0.6f, 0); }
			else if (hit == UI_NPC_ADD) { spawnNpcs(5); audioPlay(SND_CLICK, 0.6f, 0); }
			else if (hit == UI_NPC_CLEAR) { fightStart(); audioPlay(SND_CLICK, 0.6f, 0); }
			else if (hit >= UI_ATK && hit < UI_ATK + 3) fightAttack(hit - UI_ATK);
			else if (hit == UI_FLEE) fightEnd();
			uiDirty = true;
			down &= ~KEY_TOUCH;
		}
		if (down & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			if (tp.px >= 214) setCap(!cap30);
			else if (tp.px >= 107 && rtsMode) { waterTint = (waterTint + 1) % NTINT; tintChanged = true; }
			else if (tp.px >= 107) {
				radius = (radius + 1) % 3;
				if (!autoFar) farDist = radius == 0 ? 7000.0f : radius == 1 ? 12000.0f : 18000.0f;
			} else if (!rtsMode) autoFar = !autoFar;
			else fxMode = (fxMode + 1) % 4;
		}
		if (!rtsMode && (held & KEY_DUP)) speed *= 1.0f + dt * 1.5f;
		if (!rtsMode && (held & KEY_DDOWN)) speed /= 1.0f + dt * 1.5f;
		if (rtsMode) {   // D-pad left: the camera (RTS / TPP / FPP), right: the torch; up/down: zoom (FPP: look)
			if (down & KEY_DLEFT) { camMode = (camMode + 1) % NCAM; camPitch = camMode == CAM_TPP ? -0.22f : 0; setCamLimits(); }
			if (down & (KEY_DLEFT | KEY_DRIGHT)) uiDirty = true;
			if (down & KEY_DRIGHT) { torchOn = !torchOn; invDirty = true; if (!torchOn) audioPlay(SND_TORCHOUT, 0.7f, 0); }
			if (invOpen) {   // the inventory takes the D-pad's up / down
				int n = invCount();
				if (down & KEY_DDOWN) { invSel = (invSel + 1) % n; invDirty = true; }
				if (down & KEY_DUP) { invSel = (invSel + n - 1) % n; invDirty = true; }
				held &= ~(KEY_DUP | KEY_DDOWN);
			}
			float zin = ((held & KEY_DDOWN) ? 1.0f : 0) - ((held & KEY_DUP) ? 1.0f : 0);
			if (camMode == CAM_RTS && (held & KEY_X)) rtsPitch = fminf(-0.35f, fmaxf(-1.48f, rtsPitch + zin * 0.9f * dt));
			else if (camMode == CAM_RTS) rtsDist = fminf(1300.0f, fmaxf(160.0f, rtsDist * (1.0f + zin * 1.2f * dt)));
			else if (camMode == CAM_TPP) tppDist = fminf(700.0f, fmaxf(90.0f, tppDist * (1.0f + zin * 1.2f * dt)));
			else camPitch = fminf(1.3f, fmaxf(-1.3f, camPitch - zin * 1.2f * dt));
		} else if (autoFar) {
			if (held & KEY_DRIGHT) gpuBudget += 6.0f * dt;
			if (held & KEY_DLEFT) gpuBudget -= 6.0f * dt;
			if (gpuBudget < 6.0f) gpuBudget = 6.0f;
			if (gpuBudget > 30.0f) gpuBudget = 30.0f;
		} else {
			if (held & KEY_DRIGHT) farDist += 6000.0f * dt;
			if (held & KEY_DLEFT) farDist -= 6000.0f * dt;
			if (farDist < 2000.0f) farDist = 2000.0f;
			if (farDist > 40000.0f) farDist = 40000.0f;
		}

		circlePosition cp;
		hidCircleRead(&cp);
		if (bench) cp.dx = cp.dy = 0;
		float lookX = 0, lookY = 0;
		if (rtsMode && !bench) {   // circle pad walks, B runs, L/R (or Y/A) turn the camera round the character
			float pmx = abs(cp.dx) > 15 ? cp.dx / 156.0f : 0, pmy = abs(cp.dy) > 15 ? cp.dy / 156.0f : 0;
			float turn = ((held & KEY_R) ? 1.0f : 0) - ((held & KEY_L) ? 1.0f : 0);
			// X: the inventory; A there: take / put on; else A attacks (on land) or dives (held, in the
			// water), Y jumps; L+A: five NPCs more, L+Y: all of them gone
			if (chOk && useConsole && (down & KEY_X)) { invOpen = !invOpen; invDirty = true; consoleClear(); }
			bool combo = (held & KEY_L) != 0;
			if (camMode == CAM_FPP) curOn = false;
			if (curOn && !fight.on && !invOpen) {   // the cursor: the pad moves it, at the edge the camera turns
				curX += pmx * 260.0f * dt; curY -= pmy * 200.0f * dt;
				if (curX < 2) { curX = 2; if (pmx < 0) turn += pmx; }
				if (curX > 398) { curX = 398; if (pmx > 0) turn += pmx; }
				if (curY < 2) curY = 2;
				if (curY > 238) curY = 238;
				pmx = pmy = 0;
				if (!combo && (down & (KEY_A | KEY_Y)) && curKind != CK_NONE) {
					bool y = (down & KEY_Y) != 0;
					logw("    cursor %c: kind %d #%d at %.0f %.0f %.0f, %.0f away\n", y ? 'Y' : 'A', curKind, curIdx, curP[0], curP[1], curP[2], curDist);
					if (curKind == CK_NPC && actors[curIdx].dead) { if (!y) gameMessage("Zwłoki"); }
					else if (curKind == CK_GROUND && y) { }
					else { walkStart(curKind, curIdx, curP, y); audioPlay(SND_CLICK, 0.4f, 0); }
					down &= ~(KEY_A | KEY_Y);
				}
				if (down & KEY_B) walkTo.on = false;
			} else if (pmx != 0 || pmy != 0) walkTo.on = false;   // walking by hand stops the walk to a point
			if (camMode == CAM_FPP && !combo && !fight.on && !invOpen && (down & KEY_A) && curKind == CK_NPC && curDist < 160.0f && !actors[curIdx].dead) {
				walkStart(CK_NPC, curIdx, curP, false);   // (in reach: talks at once)
				down &= ~KEY_A;
			}
			bool walkRun = false;
			if (!fight.on && walkStep(dt, &pmx, &pmy, &walkRun)) { if (walkRun) held |= KEY_B; }
			if (combo && (down & KEY_A)) { spawnNpcs(5); invDirty = true; uiDirty = true; }
			else if (combo && (down & KEY_Y)) { fightEnd(); clearNpcs(); invDirty = true; uiDirty = true; }
			else if (combo && (down & KEY_X)) {   // L+X: the scripts again (from the SD card: edited, uploaded, no build)
				scriptsOk = scriptInit();
				char m[96];
				if (scriptsOk) snprintf(m, sizeof m, "Skrypty wczytane od nowa (%s)", strncmp(scriptSource, "sdmc", 4) ? "romfs" : "karta SD");
				else snprintf(m, sizeof m, "%s", scriptError());
				gameMessage(m);
			}
			else if (invOpen && (down & KEY_A)) invApply(invSel);
			else if (!invOpen && !fight.on && (down & KEY_A) && objTarget >= 0) objActivate(objTarget);
			else if (!invOpen && !swimming && !fight.on && (down & KEY_A) && chOk && PL->act < 0) {
				PL->act = PL->weapon >= 0 ? A_SLASH1H : A_SLASHHH;
				audioPlay((rand() & 1) ? SND_SWING1 : SND_SWING2, 0.8f, 0);
			}
			if (fight.on) {
				if (down & KEY_A) fightAttack(0);
				else if (down & KEY_X) fightAttack(1);
				else if (down & KEY_Y) fightAttack(2);
				else if (down & KEY_B) fightEnd();
				pmx = pmy = 0; down &= ~(KEY_A | KEY_Y); held &= ~KEY_B;
				uiDirty = true;
			}
			bool dive = !combo && !invOpen && swimming && (held & KEY_A);
			bool jump = !combo && (down & KEY_Y);
			rtsYaw += turn * 1.8f * dt;
			chrUpdate(dt, pmx, pmy, (held & KEY_B) != 0, dive, jump);
			held &= ~(KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R);
			cp.dx = cp.dy = 0;
		}
		if (held & KEY_A) lookX += 1;
		if (held & KEY_Y) lookX -= 1;
		if (held & KEY_X) lookY += 1;
		if (held & KEY_B) lookY -= 1;
		yaw += lookX * 1.8f * dt;
		pitch += lookY * 1.4f * dt;
		if (pitch > 1.5f) pitch = 1.5f;
		if (pitch < -1.5f) pitch = -1.5f;
		float fwd[3] = { cosf(pitch) * sinf(yaw), cosf(pitch) * cosf(yaw), sinf(pitch) };
		float right[3] = { cosf(yaw), -sinf(yaw), 0 };
		float mv = abs(cp.dy) > 15 ? cp.dy / 156.0f : 0, st = abs(cp.dx) > 15 ? cp.dx / 156.0f : 0;
		for (int i = 0; i < 3; i++) eye[i] += (fwd[i] * mv + right[i] * st) * speed * dt;
		if (held & KEY_R) eye[2] += speed * dt;
		if (held & KEY_L) eye[2] -= speed * dt;
		if (bench) benchCamera(dt, fwd);
		else if (rtsMode) rtsCamera(fwd);
		curKind = CK_NONE; curLabel[0] = 0;
		if (rtsMode && !bench && !fight.on && (camMode == CAM_FPP || curOn)) {
			if (camMode == CAM_FPP) cursorPick(fwd, 200.0f, 120.0f, 60.0f);
			else cursorPick(fwd, curX, curY, 60.0f);
		}
		if (invOpen && invDirty && !bench) invDraw();
		if (rtsMode && !bench) soundUpdate();
		if (fightUpdate(dt)) uiDirty = true;
		if (objsOk) {   // doors swing; the object in reach (RTS / TPP / FPP: the character's)
			objUpdate(dt);
			int t = rtsMode && !bench && !fight.on ? objNearest(chr, chrYaw, 130.0f) : -1;
			if (camMode == CAM_FPP && curKind == CK_OBJ && curDist < 220.0f) t = curIdx;
			else if (camMode == CAM_FPP && curKind != CK_NONE && curDist < 220.0f) t = -1;   // looking at something else
			if (t != objTarget) { objTarget = t; uiDirty = true; }
		}
		scriptMs = 0; scriptCalls = 0;
		if (scriptsOk && !bench) scriptFrame();
		if (noteTime > 0 && (noteTime -= dt) <= 0) { noteText[0] = 0; uiDirty = true; }
		if (fight.on && rtsMode) {   // the camera over the player's shoulder, at the enemy
			float want = chrYaw + 0.5f, d = want - rtsYaw;
			while (d > (float)M_PI) d -= 2 * (float)M_PI;
			while (d < -(float)M_PI) d += 2 * (float)M_PI;
			rtsYaw += d * fminf(1.0f, dt * 3.0f);
		}
		if (chOk && rtsMode) {   // the player's state into its actor, then every actor's pose
			u64 a0 = ticks();
			PL->pos[0] = chr[0]; PL->pos[1] = chr[1]; PL->pos[2] = chr[2];
			PL->yaw = chrYaw; PL->pitch = chrPitch;
			if (PL->swim != swimming || PL->torch != torchOn) PL->dirty = true;
			PL->swim = swimming; PL->air = chrAir; PL->torch = torchOn;
			for (int i = 0; i < MAX_ACTORS; i++) {   // far NPCs (not drawn) are not posed (hardware: 0.09 ms each)
				Actor* a = &actors[i];
				if (!a->used) continue;
				if (i > 0) {
					float dx = a->pos[0] - eye[0], dy = a->pos[1] - eye[1];
					if (dx * dx + dy * dy > (farDist + 200.0f) * (farDist + 200.0f)) { a->t += dt; continue; }
				}
				if (i > 0 && !a->dead) {   // NPCs stand on the ground (or a floor a step up), not where they were put
					float g;
					if (standAt(a->pos[0], a->pos[1], a->pos[2], 45.0f, &g) && g > -SWIM_DEPTH) a->pos[2] = g;
				}
				chAnimate(a, dt);
			}
			if (chTorchGroup >= 0) chFlamePos(PL, torchPos);
			chAnimMs = msSince(a0);
		}
		chDrawMs = 0;
		// the torch's flicker: two slow waves and a fast one
		torchFlicker = 0.75f + 0.12f * sinf(fxTime * 7.3f) + 0.08f * sinf(fxTime * 13.1f + 1.3f) + 0.05f * sinf(fxTime * 29.0f);
		// under the water? (the camera below the surface where there is water over the ground)
		{
			float g;
			bool uw = rtsMode && eye[2] < -2.0f && groundAt(eye[0], eye[1], &g) && g < eye[2];
			if (uw != underwater) { underwater = uw; setCamLimits(); }
			fogColour = underwater ? 0xFF000000 | 0x58 << 16 | 0x4C << 8 | 0x1C : 0xFF000000 | SKY_B << 16 | SKY_G << 8 | SKY_R;
			inInterior = interiorAt(chr[0], chr[1]);
			if (inInterior >= 0) {   // inside: the cell's own fog (dark), the view short
				u32 f = interiors[inInterior].fog;
				fogColour = 0xFF000000 | ((f & 0xFF) / 2) | ((f >> 8 & 0xFF) / 2) << 8 | ((f >> 16 & 0xFF) / 2) << 16;
				if (farDist > 3000.0f) farDist = 3000.0f;
			}
		}
		if (teleported) { vel[0] = vel[1] = 0; prevEye[0] = chr[0]; prevEye[1] = chr[1]; teleported = false; }
		if (dt > 0) {   // smoothed velocity for the loader's prediction
			float k = dt * 3.0f; if (k > 1) k = 1;
			// RTS: the character's, not the camera's (turning round it is not going anywhere: hardware
			// test 6 loaded the whole 5 x 5 while standing)
			const float* at = rtsMode ? chr : eye;
			vel[0] += ((at[0] - prevEye[0]) / dt - vel[0]) * k;
			vel[1] += ((at[1] - prevEye[1]) / dt - vel[1]) * k;
		}
		prevEye[0] = rtsMode ? chr[0] : eye[0]; prevEye[1] = rtsMode ? chr[1] : eye[1];

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		u64 c0 = ticks();
		float setupMs = streamCells();   // after FrameBegin: the GPU is done with the last frame's buffers
		if (tintChanged) { rebuildWater(); tintChanged = false; }

		fxTime += dt;
		float slider = bench ? 0 : osGet3DSliderState();
		bool stereo = slider > 0.02f && topR;
		bool aa = fxMode == 1 && !stereo && topAA && !bench;
		C3D_RenderTarget* left = aa ? topAA : top;
		if (left != curLeft) {
			C3D_RenderTargetSetOutput(left, GFX_TOP, GFX_LEFT, aa ? AA_FLAGS : DISPLAY_TRANSFER_FLAGS);
			curLeft = left;
		}
		// the focal plane: the character in RTS mode (it stands on the screen, the world round it)
		float screenD = !rtsMode ? 600.0f : camMode == CAM_RTS ? rtsDist : camMode == CAM_TPP ? tppDist : 300.0f;
		float iod = slider * screenD / 10.0f;
		C3D_Mtx view;
		// under the water the view sways a little (the up tilts, the field of view breathes)
		float sway = underwater ? 0.025f * sinf(fxTime * 1.7f) : 0, fov = underwater ? 62.0f + 1.5f * sinf(fxTime * 2.3f) : 60.0f;
		Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(fwd[0], fwd[1], fwd[2]),
		           FVec3_New(sway * cosf(yaw), -sway * sinf(yaw), 1), false);
		for (int e = 0; e < (stereo ? 2 : 1); e++) {
			C3D_Mtx proj, mvp;
			if (stereo)
				Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(fov), C3D_AspectRatioTop, 8.0f, farDist + 1000.0f,
				                    e == 0 ? -iod : iod, screenD, false);
			else
				Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(fov), C3D_AspectRatioTop, 8.0f, farDist + 1000.0f, false);
			Mtx_Multiply(&mvp, &proj, &view);
			C3D_RenderTarget* rt = e == 0 ? left : topR;
			u32 clr = (fogColour & 0xFF) << 24 | (fogColour >> 8 & 0xFF) << 16 | (fogColour >> 16 & 0xFF) << 8 | 0xFF;
			C3D_RenderTargetClear(rt, C3D_CLEAR_ALL, clr, 0);
			C3D_FrameDrawOn(rt);
			if (inInterior >= 0 && rtsMode && camMode != CAM_FPP) clipAbove(&mvp, chr[2] + (camMode == CAM_RTS ? 160.0f : 230.0f));
			else clipOff();
			int d0 = statDraws; u32 t0s = statTris;
			drawWorld(&mvp, fwd);   // (resets the stats: the second eye's adds to the first's below)
			drawObjects(&mvp, fwd);
			if (rtsMode) drawActors(&mvp, fwd);
			if (rtsMode && fxMode != FX_PLAIN && !swimming) drawShadow(&mvp, fwd);
			if (fxMode == 0) drawCaustics(&mvp, fwd);
			if (underwater) drawWaterBelow(&mvp, fwd);
			else if (fxMode != FX_PLAIN) drawWater(&mvp, fwd);
			if (rtsMode && torchOn && !swimming && camMode != CAM_FPP) drawFlame(&mvp, fwd);
			if (e == 1) { statDraws += d0; statTris += t0s; }
		}
		clipOff();
		if (uiOn && rtsMode && !bench && !fight.on && (camMode == CAM_FPP || curOn)) {
			for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
			bool fpp = camMode == CAM_FPP;
			for (int e = 0; e < (stereo ? 2 : 1); e++)
				uiDrawTop(e == 0 ? left : topR, fpp ? 1 : 2, fpp ? 200.0f : curX, fpp ? 120.0f : curY, curLabel,
				          !fpp && walkTo.on && walkTo.kind == CK_GROUND);
			for (int i = 3; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
		}
		if (uiOn && uiDirty) {
			for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));   // (the 3D's fog stage off: it tinted the window)
			uiFill();
			uiDraw(&uiState);
			uiDirty = false;
			for (int i = 3; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));   // (citro2d's stages off for the 3D)
		}
		cpuMs = msSince(c0);
		C3D_FrameEnd(0);
		float gpuMs = C3D_GetDrawingTime();   // the previous frame's
		pace(gpuMs);

		if (bench) benchAfterFrame(rawMs, cpuMs, gpuMs, setupMs, &quit);

		cpuAcc += cpuMs;
		if (cpuMs > cpuMax) cpuMax = cpuMs;
		frames++;
		if (now - fpsT > SYSCLOCK_ARM11 / 2) {
			float secs = (now - fpsT) / (float)SYSCLOCK_ARM11;
			fps = frames / secs;
			float cpuAvg = cpuAcc / frames;
			cpuMaxShown = cpuMax;
			fpsT = now; frames = 0; cpuAcc = 0; cpuMax = 0;

			int loaded = 0; u32 ltris = 0, lbytes = 0;
			for (int i = 0; i < MAX_CELLS; i++) if (cells[i].used) { loaded++; ltris += cells[i].tris; lbytes += cells[i].size; }
			++playTick;
			if (!bench && playTick % 4 == 0) {
				logw("%s fx%d 3d%.0f fps %.1f cpu %.2f (max %.2f) gpu %.2f draws %d tris %lu | r%d far %.0f%s budget %.0f | cells %d missing %d | load read %.0f setup %.1f ms #%d fail %d | free %lu KB | cell %d,%d | char %s %d verts | npcs %d anim %.2f draw %.2f ms | obj draws %d script %.2f ms | cam %s%s%s\n",
				     rtsMode ? "rts" : "free", fxMode, osGet3DSliderState() * 100, fps, cpuAvg, cpuMaxShown, gpuMs, statDraws, statTris, radius, farDist, autoFar ? " auto" : "", gpuBudget,
				     loaded, cellsMissing, lastIoMs, lastSetupMs, loadsTotal, loadFailed, linearSpaceFree() / 1024,
				     (int)floorf(eye[0] / CELL), (int)floorf(eye[1] / CELL),
				     chOk ? "pieces" : "-", PL->verts, nNpc, chAnimMs, chDrawMs, statObjDraws, scriptMs,
				     CAM_NAMES[camMode], torchOn ? " torch" : "", underwater ? " underwater" : swimming ? " swim" : "");
				if (playTick % 60 == 0) logFlush();   // every 30 s
			}
			// The console's text costs ~7 ms of CPU on the Old 3DS (hardware test 2: a 40 ms frame at each
			// refresh): not during the benchmark, and once a second otherwise.
			if (uiOn) {
				if (!audioOk()) snprintf(uiState.status, sizeof uiState.status, "Brak dzwieku: uruchom raz DSP1 (3ds/DSP1)");
				else snprintf(uiState.status, sizeof uiState.status, "%.0f fps   CPU %.1f   GPU %.1f ms   NPC %d", fps, cpuAvg, gpuMs, nNpc);
				uiDirty = true;
			}
			if (!useConsole || bench || (playTick & 1) || invOpen) continue;
			printf("\x1b[3;0H");
			printf("FPS        %5.1f  (cap %d)       \n", fps, cap30 ? 30 : 60);
			printf("CPU frame  %5.2f ms (max %5.2f)  \n", cpuAvg, cpuMaxShown);
			printf("GPU draw   %5.2f ms  budget %4.1f  \n", gpuMs, gpuBudget);
			printf("draws %4d  culled %4d small %3d \n", statDraws, statCulled, statSmall);
			printf("tris drawn %6lu             \n", statTris);
			printf("\n");
			printf("looks: %s  3D %3.0f%%   \n",
			       fxMode == 0 ? "fx+caustics " : fxMode == 1 ? "fx ripples  " : fxMode == 2 ? "fx calm     " : "plain       ", osGet3DSliderState() * 100);
			printf("water: %s (Dpad L/R)  \n", TINT_NAMES[waterTint]);
			if (chOk) printf("NPCs %2d  pose %4.2f draw %5.2f ms \n", nNpc, chAnimMs, chDrawMs);
			if (chOk) printf("  player %d meshes %d verts     \n", PL->nshown, PL->verts);
			printf("radius %d (%dx%d)  far %5.0f %s  \n", radius, radius * 2 + 1, radius * 2 + 1, farDist, autoFar ? "AUTO" : "man ");
			printf("cells loaded %2d  missing %d     \n", loaded, cellsMissing);
			printf("cell data %5lu KB  tris %7lu  \n", lbytes / 1024, ltris);
			printf("load: read %4.0f setup %4.1f ms #%d \n", lastIoMs, lastSetupMs, loadsTotal);
			printf("loader %s  fails %d          \n", job.state == JOB_READING ? "busy" : "idle", loadFailed);
			printf("linear free %5lu KB          \n", linearSpaceFree() / 1024);
			printf("\n");
			printf("cell %3d,%3d  speed %5.0f     \n", (int)floorf(eye[0] / CELL), (int)floorf(eye[1] / CELL), speed);
			printf("pos %7.0f %7.0f %6.0f \n", eye[0], eye[1], eye[2]);
			printf("%s\n", bench ? "\x1b[33mBENCHMARK running\x1b[0m        " : "                          ");
			if (rtsMode)
				printf("\x1b[2m%s: pad walk B run L/R turn  \nA attack/dive Y jump X inventory\nDpad < cam > torch ^v zoom     \nL+A 5 NPCs  L+Y no NPCs        \n%s\x1b[0m",
				       CAM_NAMES[camMode], swimming ? (underwater ? "swimming, under water         " : "swimming                      ")
				                                    : torchOn ? "torch                         " : "                              ");
			else
				printf("\x1b[2mpad move  ABXY look  L/R down/up\nDpad U/D speed  L/R budget|far\nSELECT RTS camera  START quit  \ntouch: [auto][radius][30/60]   \x1b[0m");
		}
	}

	if (bPhase != B_OFF) {
		logw("done\n");
		logFlush();
		// the next start is the manual mode; rename bench.done back to bench.txt to run it again
		remove("sdmc:/3ds/mwcell3d/bench.done");
		rename("sdmc:/3ds/mwcell3d/bench.txt", "sdmc:/3ds/mwcell3d/bench.done");
	} else logFlush();
	if (logFile) fclose(logFile);

	if (loaderThread) {
		while (job.state == JOB_READING) svcSleepThread(1000000);
		loaderQuit = true;
		LightEvent_Signal(&jobEvent);
		threadJoin(loaderThread, U64_MAX);
		threadFree(loaderThread);
	}
	if (job.state == JOB_DONE) linearFree(job.data);
done:
	for (int i = 0; i < MAX_CELLS; i++) if (cells[i].used) unloadCell(&cells[i]);
	for (int i = 0; i < ntex; i++) C3D_TexDelete(&texs[i]);
	if (shV) linearFree(shV);
	if (shI) linearFree(shI);
	if (rippleOk) C3D_TexDelete(&rippleTex);
	if (detailOk) C3D_TexDelete(&detailTex);
	shaderProgramFree(&wProg);
	DVLB_Free(wDvlb);
	if (chrV) linearFree(chrV);
	if (chrI) linearFree(chrI);
	shaderProgramFree(&prog);
	DVLB_Free(dvlb);
	C3D_Fini();
	audioExit();
	romfsExit();
	gfxExit();
	return 0;
}
