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
#include "world_shbin.h"
#include "water_shbin.h"

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
	u32 colofs, ncol;   // MWC2: collision boxes (8 floats each: centre x y, axis cos sin, half sizes, z from, to)
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
#define RTS_DIST 700.0f
#define RTS_FAR 2600.0f     // the farthest corner of the view on flat ground is ~1800 away; the rest for tall things
#define RTS_TARGET_Z 70.0f  // the camera looks at the character's chest
#define RTS_LOAD 3500.0f    // RTS: cells nearer the character than this are loaded,
#define RTS_DROP 6500.0f    // ... farther than this unloaded (instead of the 3 x 3 rings)
#define WALK_SPEED 170.0f
#define RUN_SPEED 420.0f
static bool rtsMode = true;
static float rtsYaw;                     // the camera's turn round the character
static float chr[3];                     // the character's feet
static float chrYaw, chrPhase, chrSwing; // facing; walk cycle phase and its strength (0 standing .. 1)
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
	u8* buf = readFile("romfs:/textures.bin", &size);
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
	u8* buf = readFile("romfs:/index.bin", &size);
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

static void unloadCell(Cell* c)
{
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
		snprintf(path, sizeof path, "romfs:/cells/%d_%d.bin", job.gx, job.gy);
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
	if (!c || (memcmp(h->magic, "MWC1", 4) && memcmp(h->magic, "MWC2", 4))) { linearFree(job.data); return -1; }
	c->used = true; c->gx = job.gx; c->gy = job.gy; c->data = job.data; c->size = job.size; c->hdr = h;
	c->b = (BatchRec*)(job.data + h->bofs);
	c->col = NULL; c->ncol = 0;
	if (!memcmp(h->magic, "MWC2", 4) && h->ncol && h->colofs + h->ncol * 32 <= job.size) {
		c->col = (const float*)(job.data + h->colofs);
		c->ncol = h->ncol;
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
				if (rd > RTS_LOAD + 2500.0f) continue;
				inner = rd < RTS_LOAD;
			} else inner = abs(dx) <= radius && abs(dy) <= radius;
			const Avail* a = findAvail(gx, gy);
			if (!a || findCell(gx, gy)) continue;
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
		if (rtsMode ? rectDist(c->gx, c->gy, chr[0], chr[1]) > RTS_DROP
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
	C3D_TexEnvColor(env, 0xFF000000 | SKY_B << 16 | SKY_G << 8 | SKY_R);
}

static void drawWorld(const C3D_Mtx* mvp, const float fwd[3])
{
	C3D_BindProgram(&prog);
	C3D_SetAttrInfo(&attr);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp, mvp);
	float fs = farDist * fogStart, fe = farDist, k = 1.0f / (fe - fs);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, -fwd[0] * k, -fwd[1] * k, -fwd[2] * k, fe * k);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uDetail, 0, 0, 1.0f / 256.0f, 0);
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

// tools/chrbake.py: the skeleton's animations as the bones' matrices per frame, the body and the
// clothes / armour as meshes whose vertices hang on up to 4 bones (each influence: the bone, the
// weight, the position and normal in that bone's space). Here: the pose of the frame (two frames
// blended, and the last animation faded out over 0.2 s), every shown mesh skinned on the CPU into
// one buffer (character space turned by the facing, lit by the sun) and drawn with the world shader.
typedef struct { u8 bone, pad[3]; float w, p[3], n[3]; } CInfl;
typedef struct { s16 u, v; u8 n, pad[3]; } CVert;
typedef struct {
	u16 tex; u8 alpha, pad;
	u32 nv, ninf, nidx;
	float col[3];
	CVert* v; CInfl* inf; u16* idx;
	u32 out;              // its first vertex in chOut
	C3D_BufInfo buf;
} CMesh;
typedef struct { char name[16]; u32 frames; float fps, speed; const float* m; } CAnim;
typedef struct { char name[24]; u8 helmet, n; const u8* parts; } CItem;   // parts: n x { u8 slot, u8 pad, s16 group }
typedef struct { char name[16]; u8 n; const u8* items; } COutfit;

static u8* chData;
static bool chOk;
static int chBones, chNAnim, chNMesh, chNGroup, chNItem, chNOutfit;
static CAnim chAnim[8];
static CMesh* chMesh;
static const u16** chGroup;     // per group: u16 n, then n mesh ids
static s16 chBody[27];
static CItem* chItem;
static COutfit* chOutfit;
static Vtx* chOut;
static u32 chOutN;
static int chOutfitSel = 1, chHelmet = 1;
static int chShown[160], chNShown;
static float chPose[64][12], chPrevPose[64][12];
static int chCur = 0, chPrev = -1;
static float chTime, chPrevTime, chFade;
static float chSpeedNow;        // units/s the character moves at (for the animation)
static bool chRun;
static u32 chSkinFrame = 0xFFFFFFFF, chFrameNo;
static float chSkinMs;
static int chShownVerts;

static bool loadCharacter(void)
{
	u32 size;
	chData = readFile("romfs:/char.bin", &size);
	if (!chData || memcmp(chData, "MWCH", 4)) { free(chData); chData = NULL; return false; }
	const u8* p = chData + 4;
	u32 h[6];
	memcpy(h, p, 24); p += 24;
	chBones = h[0]; chNAnim = h[1]; chNMesh = h[2]; chNGroup = h[3]; chNItem = h[4]; chNOutfit = h[5];
	if (chBones > 64 || chNAnim > 8) return false;
	for (int i = 0; i < chNAnim; i++) {
		memcpy(chAnim[i].name, p, 16); p += 16;
		memcpy(&chAnim[i].frames, p, 4); memcpy(&chAnim[i].fps, p + 4, 4); memcpy(&chAnim[i].speed, p + 8, 4); p += 12;
		chAnim[i].m = (const float*)p;
		p += chAnim[i].frames * chBones * 12 * 4;
	}
	chMesh = calloc(chNMesh, sizeof(CMesh));
	u32 total = 0;
	for (int i = 0; i < chNMesh; i++) {
		CMesh* m = &chMesh[i];
		memcpy(&m->tex, p, 2); m->alpha = p[2];
		memcpy(&m->nv, p + 4, 4); memcpy(&m->ninf, p + 8, 4); memcpy(&m->nidx, p + 12, 4); memcpy(m->col, p + 16, 12);
		p += 28;
		m->v = (CVert*)p; p += m->nv * 8;
		m->inf = (CInfl*)p; p += m->ninf * 32;
		m->idx = (u16*)linearAlloc(m->nidx * 2 + 4);
		if (!m->idx) return false;
		memcpy(m->idx, p, m->nidx * 2);
		GSPGPU_FlushDataCache(m->idx, m->nidx * 2);
		p += (m->nidx * 2 + 3) & ~3u;
		m->out = total; total += m->nv;
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
	chOut = (Vtx*)linearAlloc(total * sizeof(Vtx));
	if (!chOut) return false;
	chOutN = total;
	for (int i = 0; i < chNMesh; i++) {
		BufInfo_Init(&chMesh[i].buf);
		BufInfo_Add(&chMesh[i].buf, chOut + chMesh[i].out, sizeof(Vtx), 3, 0x210);
	}
	if (chOutfitSel >= chNOutfit) chOutfitSel = 0;
	chOk = true;
	return true;
}

// What the slots show: the body, unless the outfit's items cover them (a helmet off: its slots
// left as they are); any helmet hides the hair.
static void chCompose(void)
{
	s16 slot[27];
	memcpy(slot, chBody, sizeof slot);
	bool hideHair = false;
	const COutfit* o = &chOutfit[chOutfitSel];
	for (int k = 0; k < o->n; k++) {
		const CItem* it = &chItem[o->items[k]];
		if (it->helmet && !chHelmet) continue;
		if (it->helmet) hideHair = true;
		for (int j = 0; j < it->n; j++) {
			u8 sl = it->parts[j * 4];
			s16 g; memcpy(&g, it->parts + j * 4 + 2, 2);
			if (sl < 27) slot[sl] = g;
		}
	}
	if (hideHair) slot[1] = -1;
	chNShown = 0;
	chShownVerts = 0;
	for (int sl = 0; sl < 27; sl++) {
		int g = slot[sl];
		if (g < 0 || g >= chNGroup) continue;
		bool dup = false;
		for (int k = 0; k < sl; k++) if (slot[k] == g) { dup = true; break; }
		if (dup) continue;
		const u16* gr = chGroup[g];
		for (int k = 0; k < gr[0] && chNShown < 160; k++) {
			chShown[chNShown++] = gr[1 + k];
			chShownVerts += chMesh[gr[1 + k]].nv;
		}
	}
}

static void chSample(int a, float t, float out[][12])
{
	const CAnim* an = &chAnim[a];
	float f = t * an->fps;
	int n = an->frames;
	f = fmodf(f, (float)n);
	if (f < 0) f += n;
	int i = (int)f, j = (i + 1) % n;
	float k = f - i;
	const float* A = an->m + (size_t)i * chBones * 12;
	const float* B = an->m + (size_t)j * chBones * 12;
	for (int b = 0; b < chBones * 12; b++) out[0][b] = A[b] + (B[b] - A[b]) * k;
}

// The animation by the speed: still -> idle, else walk or run played at the speed it was made for.
static void chAnimate(float dt)
{
	int want = 0;
	if (chSpeedNow > 20.0f) want = (chRun && chNAnim > 2) ? 2 : (chNAnim > 1 ? 1 : 0);
	if (want != chCur) {
		chPrev = chCur; chPrevTime = chTime; chFade = 1.0f;
		chCur = want; chTime = 0;
	}
	float rate = 1.0f;
	if (chAnim[chCur].speed > 1.0f) {
		rate = chSpeedNow / chAnim[chCur].speed;
		if (rate < 0.3f) rate = 0.3f;
		if (rate > 2.5f) rate = 2.5f;
	}
	chTime += dt * rate;
	chSample(chCur, chTime, chPose);
	if (chFade > 0 && chPrev >= 0) {
		chPrevTime += dt;
		chSample(chPrev, chPrevTime, chPrevPose);
		for (int b = 0; b < chBones * 12; b++) chPose[0][b] += (chPrevPose[0][b] - chPose[0][b]) * chFade;
		chFade -= dt * 5.0f;
	}
}

// The shown meshes into chOut: turned by the facing, lit (ambient + sun, as the baker lights).
static void chSkin(void)
{
	u64 t0 = ticks();
	const float SUNX = -0.35f, SUNY = 0.45f, SUNZ = 0.82f;
	const float sl = 1.0f / sqrtf(SUNX * SUNX + SUNY * SUNY + SUNZ * SUNZ);
	float cy = cosf(chrYaw), sy = sinf(chrYaw);
	// the sun in the character's space (instead of turning every normal)
	float lx = (SUNX * cy - SUNY * sy) * sl, ly = (SUNX * sy + SUNY * cy) * sl, lz = SUNZ * sl;
	for (int s = 0; s < chNShown; s++) {
		CMesh* m = &chMesh[chShown[s]];
		Vtx* o = chOut + m->out;
		const CInfl* in = m->inf;
		for (u32 v = 0; v < m->nv; v++) {
			float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0;
			for (int k = 0; k < m->v[v].n; k++, in++) {
				const float* B = chPose[in->bone];
				float w = in->w;
				px += w * (B[0] * in->p[0] + B[1] * in->p[1] + B[2] * in->p[2] + B[3]);
				py += w * (B[4] * in->p[0] + B[5] * in->p[1] + B[6] * in->p[2] + B[7]);
				pz += w * (B[8] * in->p[0] + B[9] * in->p[1] + B[10] * in->p[2] + B[11]);
				nx += w * (B[0] * in->n[0] + B[1] * in->n[1] + B[2] * in->n[2]);
				ny += w * (B[4] * in->n[0] + B[5] * in->n[1] + B[6] * in->n[2]);
				nz += w * (B[8] * in->n[0] + B[9] * in->n[1] + B[10] * in->n[2]);
			}
			// facing: yaw 0 = +y (like the camera): x' = x cos + y sin, y' = -x sin + y cos
			o->p[0] = (s16)(px * cy + py * sy); o->p[1] = (s16)(-px * sy + py * cy); o->p[2] = (s16)pz; o->p[3] = 0;
			o->t[0] = m->v[v].u; o->t[1] = m->v[v].v;
			float nl = nx * nx + ny * ny + nz * nz;
			float d = nl > 1e-6f ? (nx * lx + ny * ly + nz * lz) / sqrtf(nl) : 0.5f;
			if (d < 0) d = 0;
			float r = m->col[0] * (0.52f + 0.62f * d), g = m->col[1] * (0.54f + 0.58f * d), b = m->col[2] * (0.60f + 0.50f * d);
			o->c[0] = r >= 1 ? 255 : (u8)(r * 255); o->c[1] = g >= 1 ? 255 : (u8)(g * 255);
			o->c[2] = b >= 1 ? 255 : (u8)(b * 255); o->c[3] = 255;
			o++;
		}
		GSPGPU_FlushDataCache(chOut + m->out, m->nv * sizeof(Vtx));
	}
	chSkinMs = msSince(t0);
}

static void drawCharacter(void)
{
	if (!chrPlaced) return;
	if (!chOk) { drawBoxes(); return; }
	if (chSkinFrame != chFrameNo) {   // once a frame (the second eye draws the same)
		chCompose();
		chSkin();
		chSkinFrame = chFrameNo;
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uOrigin, chr[0] - eye[0], chr[1] - eye[1], chr[2] - eye[2], 1.0f / 1024.0f);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);
	setupTexEnv(255, false);
	int lastTex = -1, lastAlpha = -1;
	for (int s = 0; s < chNShown; s++) {
		CMesh* m = &chMesh[chShown[s]];
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
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
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

// Would the character at (x, y, z) stand in a thing? The boxes it stands in already at (px, py)
// do not hold it (so it can always walk out of where it started).
static bool collides(float x, float y, float z, float px, float py)
{
	if (!collideOn) return false;
	for (int i = 0; i < MAX_CELLS; i++) {
		Cell* c = &cells[i];
		if (!c->used || !c->col) continue;
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

// Walking: the circle pad relative to the camera, the figure turns to where it goes. The sea
// (ground below -64) stops it, unless it is already that deep (the start can be on a pier).
static void chrUpdate(float dt, float mx, float my, bool run)
{
	float g;
	if (!chrPlaced) {
		if (groundAt(chr[0], chr[1], &g)) { chr[2] = g > -10000.0f ? g : 0; chrPlaced = true; }
		return;
	}
	float len = sqrtf(mx * mx + my * my);
	if (len > 1) { mx /= len; my /= len; len = 1; }
	float fx = sinf(rtsYaw), fy = cosf(rtsYaw);   // the camera's forward on the ground; right = (fy, -fx)
	float dx = fx * my + fy * mx, dy = fy * my - fx * mx;
	float moved = 0;
	if (len > 0.05f) {
		float sp = (run ? RUN_SPEED : WALK_SPEED) * len;
		float nx = chr[0] + dx / len * sp * dt, ny = chr[1] + dy / len * sp * dt;
		// blocked: slide along (only x, or only y)
		const float tries[3][2] = { { nx, ny }, { nx, chr[1] }, { chr[0], ny } };
		for (int t = 0; t < 3; t++) {
			float tx = tries[t][0], ty = tries[t][1];
			if (t > 0 && tx == chr[0] && ty == chr[1]) continue;
			if (!groundAt(tx, ty, &g) || !(g > -64.0f || g >= chr[2])) continue;
			if (collides(tx, ty, g, chr[0], chr[1])) continue;
			moved = sqrtf((tx - chr[0]) * (tx - chr[0]) + (ty - chr[1]) * (ty - chr[1]));
			chr[0] = tx; chr[1] = ty;
			break;
		}
		// turn towards the way it goes (the shorter way round)
		float want = atan2f(dx, dy), d = want - chrYaw;
		while (d > (float)M_PI) d -= 2 * (float)M_PI;
		while (d < -(float)M_PI) d += 2 * (float)M_PI;
		float k = dt * 10.0f; if (k > 1) k = 1;
		chrYaw += d * k;
	}
	if (groundAt(chr[0], chr[1], &g) && g > -10000.0f) chr[2] = g;
	chSpeedNow = dt > 0 ? moved / dt : 0;
	chRun = run;
	// the walk cycle follows the distance: a step per ~70 units walking, longer running
	chrPhase += moved * (float)M_PI / (run ? 110.0f : 70.0f);
	float want = moved > 0 ? 1.0f : 0.0f, k = dt * 6.0f; if (k > 1) k = 1;
	chrSwing += (want - chrSwing) * k;
	if (chrSwing < 0.02f && moved == 0) chrPhase = 0;
}

// The camera behind and above the character, looking down at RTS_PITCH from RTS_DIST.
static void rtsCamera(float fwd[3])
{
	fwd[0] = cosf(RTS_PITCH) * sinf(rtsYaw); fwd[1] = cosf(RTS_PITCH) * cosf(rtsYaw); fwd[2] = sinf(RTS_PITCH);
	eye[0] = chr[0] - fwd[0] * RTS_DIST;
	eye[1] = chr[1] - fwd[1] * RTS_DIST;
	eye[2] = chr[2] + RTS_TARGET_Z - fwd[2] * RTS_DIST;
	// never inside a hill behind the character (hardware test 6, shot 2): up over the ground
	float g;
	if (groundAt(eye[0], eye[1], &g) && eye[2] < g + 80.0f) eye[2] = g + 80.0f;
	yaw = rtsYaw; pitch = RTS_PITCH;
}

static void setRts(bool on)
{
	rtsMode = on;
	if (on) {
		autoFar = false; farDist = RTS_FAR; fogStart = 0.7f;
		rtsYaw = yaw;
		if (!chrPlaced) { chr[0] = eye[0]; chr[1] = eye[1]; chr[2] = eye[2] - 200.0f; }
	} else {
		autoFar = true; fogStart = 0.45f;
		pitch = -0.3f;
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
static void saveShot(const char* path)
{
	u16 w, h;
	u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
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
enum { B_OFF, B_STATIC, B_WALK, B_RTS, B_DONE };
static int bPhase = B_OFF, bAngle = -1, bFrame;
static float bCpu, bCpuMax, bGpu, bDraws, bTris, bFrameMs;
static float startEye[3], startYaw;
static bool benchOnlyRts;
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

// Moves the camera along the route (before the frame).
static void benchCamera(float dt, float fwd[3])
{
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
				chSpeedNow = 0;
				if (chOk) chOutfitSel = chNOutfit - 1;   // the standing screenshot: the last outfit
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
				chSpeedNow = dt > 0 ? moved / dt : 0;
				chRun = true;
				if (chOk) chOutfitSel = (1 + (int)(wDist / (routeLen() / 4))) % chNOutfit;   // each quarter another
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

// ------------------------------------------------------------------ main

int main(void)
{
	osSetSpeedupEnable(true);
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	romfsInit();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);

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
	chrV = (Vtx*)linearAlloc(CHR_MAXV * sizeof(Vtx));
	chrI = (u16*)linearAlloc(CHR_MAXI * sizeof(u16));
	BufInfo_Init(&chrBuf);
	BufInfo_Add(&chrBuf, chrV, sizeof(Vtx), 3, 0x210);

	printf("3dsOpenworld - Morrowind cell test\n");
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
		logFlush();
		if (bPhase != B_OFF) printf("\x1b[33mBENCHMARK running (a few min),\nthis screen stays still meanwhile\x1b[0m\n");
	}
	startEye[0] = eye[0]; startEye[1] = eye[1]; startEye[2] = eye[2];
	startYaw = yaw;
	if (benchOnlyRts) benchStartRts();

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
		if (down & KEY_SELECT) setRts(!rtsMode);
		if (down & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			if (tp.px >= 214) setCap(!cap30);
			else if (tp.px >= 107) {
				radius = (radius + 1) % 3;
				if (!autoFar && !rtsMode) farDist = radius == 0 ? 7000.0f : radius == 1 ? 12000.0f : 18000.0f;
			} else if (!rtsMode) autoFar = !autoFar;
			else fxMode = (fxMode + 1) % 4;
		}
		if (held & KEY_DUP) speed *= 1.0f + dt * 1.5f;
		if (held & KEY_DDOWN) speed /= 1.0f + dt * 1.5f;
		if (rtsMode) {   // the distance is locked in RTS mode; D-pad left/right: the water's tint
			if (down & KEY_DRIGHT) { waterTint = (waterTint + 1) % NTINT; tintChanged = true; }
			if (down & KEY_DLEFT) { waterTint = (waterTint + NTINT - 1) % NTINT; tintChanged = true; }
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
			if (chOk && (down & KEY_X)) chOutfitSel = (chOutfitSel + 1) % chNOutfit;
			if (down & KEY_Y) chHelmet = !chHelmet;
			rtsYaw += turn * 1.8f * dt;
			chrUpdate(dt, pmx, pmy, (held & KEY_B) != 0);
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
		if (chOk && rtsMode) chAnimate(dt);
		chFrameNo++;
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
		float screenD = rtsMode ? RTS_DIST : 600.0f, iod = slider * screenD / 10.0f;
		C3D_Mtx view;
		Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(fwd[0], fwd[1], fwd[2]), FVec3_New(0, 0, 1), false);
		for (int e = 0; e < (stereo ? 2 : 1); e++) {
			C3D_Mtx proj, mvp;
			if (stereo)
				Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 8.0f, farDist + 1000.0f,
				                    e == 0 ? -iod : iod, screenD, false);
			else
				Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 8.0f, farDist + 1000.0f, false);
			Mtx_Multiply(&mvp, &proj, &view);
			C3D_RenderTarget* rt = e == 0 ? left : topR;
			C3D_RenderTargetClear(rt, C3D_CLEAR_ALL, SKY_R << 24 | SKY_G << 16 | SKY_B << 8 | 0xFF, 0);
			C3D_FrameDrawOn(rt);
			int d0 = statDraws; u32 t0s = statTris;
			drawWorld(&mvp, fwd);   // (resets the stats: the second eye's adds to the first's below)
			if (rtsMode) drawCharacter();
			if (rtsMode && fxMode != FX_PLAIN) drawShadow(&mvp, fwd);
			if (fxMode == 0) drawCaustics(&mvp, fwd);
			if (fxMode != FX_PLAIN) drawWater(&mvp, fwd);
			if (e == 1) { statDraws += d0; statTris += t0s; }
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
				logw("%s fx%d 3d%.0f fps %.1f cpu %.2f (max %.2f) gpu %.2f draws %d tris %lu | r%d far %.0f%s budget %.0f | cells %d missing %d | load read %.0f setup %.1f ms #%d fail %d | free %lu KB | cell %d,%d | char %s %d verts skin %.2f ms\n",
				     rtsMode ? "rts" : "free", fxMode, osGet3DSliderState() * 100, fps, cpuAvg, cpuMaxShown, gpuMs, statDraws, statTris, radius, farDist, autoFar ? " auto" : "", gpuBudget,
				     loaded, cellsMissing, lastIoMs, lastSetupMs, loadsTotal, loadFailed, linearSpaceFree() / 1024,
				     (int)floorf(eye[0] / CELL), (int)floorf(eye[1] / CELL),
				     chOk ? chOutfit[chOutfitSel].name : "-", chShownVerts, chSkinMs);
				if (playTick % 60 == 0) logFlush();   // every 30 s
			}
			// The console's text costs ~7 ms of CPU on the Old 3DS (hardware test 2: a 40 ms frame at each
			// refresh): not during the benchmark, and once a second otherwise.
			if (bench || (playTick & 1)) continue;
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
			if (chOk) printf("outfit: %-9s helm %d skin %4.1fms \n", chOutfit[chOutfitSel].name, chHelmet, chSkinMs);
			if (chOk) printf("  %d meshes %d verts          \n", chNShown, chShownVerts);
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
				printf("\x1b[2mRTS: pad walk B run L/R turn    \nX outfit Y helmet Dpad L/R water\nSELECT free camera  START quit  \ntouch: [looks][radius][30/60]  \n                                \x1b[0m");
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
	romfsExit();
	gfxExit();
	return 0;
}
