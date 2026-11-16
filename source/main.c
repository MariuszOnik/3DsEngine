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

#define CELL 8192.0f
#define HALF 4096.0f
#define MAX_CELLS 64
#define MAX_AVAIL 1024
#define MAX_ROUTE 4096

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
} __attribute__((packed)) CellHdr;

typedef struct {
	u16 tex;
	u8 flags, opacity;   // flags: 1 alpha test, 2 two-sided, 4 blended (water)
	u32 vofs, vcount, iofs, icount;
	float c[3], r;
} __attribute__((packed)) BatchRec;

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

// stats
static int statDraws, statCulled, statSmall;   // statSmall: batches of small / under-water things left out
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

static void unloadCell(Cell* c)
{
	if (c->hasT) C3D_TexDelete(&c->ttex);
	free(c->bufs);
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
	if (!c || memcmp(h->magic, "MWC1", 4)) { linearFree(job.data); return -1; }
	c->used = true; c->gx = job.gx; c->gy = job.gy; c->data = job.data; c->size = job.size; c->hdr = h;
	c->b = (BatchRec*)(job.data + h->bofs);
	c->bufs = calloc(h->nbatch, sizeof(C3D_BufInfo));
	c->tris = 0;
	for (int i = 0; i < h->nbatch; i++) {
		BufInfo_Init(&c->bufs[i]);
		BufInfo_Add(&c->bufs[i], job.data + c->b[i].vofs, 16, 3, 0x210);
		c->tris += c->b[i].icount / 3;
	}
	if (h->tsize) {   // the side in the low 12 bits, the format in the top 4
		int ts = h->tsize & 0xFFF, tf = h->tsize >> 12;
		c->hasT = texFrom(&c->ttex, ts, ts, tf, job.data + h->ttofs, 0xFFFFFFFFu, false);
	}
	GSPGPU_FlushDataCache(job.data, job.size);
	return msSince(t0);
}

// A cell is wanted within `radius` rings of the camera's cell; while moving, the next ring
// ahead too (prefetch: it is loaded before the camera gets there). Nearest to where the
// camera will be in 2 s first.
static int cellsMissing;   // wanted cells within radius not loaded yet (the world is incomplete)

static void pickJob(void)
{
	int cgx = (int)floorf(eye[0] / CELL), cgy = (int)floorf(eye[1] / CELL);
	float px = eye[0] + vel[0] * 2.0f, py = eye[1] + vel[1] * 2.0f;
	float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
	float best = 1e30f;
	const Avail* pick = NULL;
	cellsMissing = 0;
	for (int dy = -radius - 1; dy <= radius + 1; dy++)
		for (int dx = -radius - 1; dx <= radius + 1; dx++) {
			int gx = cgx + dx, gy = cgy + dy;
			bool inner = abs(dx) <= radius && abs(dy) <= radius;
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
		if (c->used && (abs(c->gx - cgx) > radius + 1 || abs(c->gy - cgy) > radius + 1)) unloadCell(c);
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

static void setupTexEnv(u8 opacity)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	C3D_TexEnvColor(env, (u32)opacity << 24 | 0xFFFFFF);
	// fog: lerp towards the sky by the vertex alpha (1 = clear)
	env = C3D_GetTexEnv(1);
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
	float fs = farDist * 0.45f, fe = farDist, k = 1.0f / (fe - fs);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, -fwd[0] * k, -fwd[1] * k, -fwd[2] * k, fe * k);

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

	statDraws = statCulled = statSmall = 0;
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
		int lastTex = -2, lastCull = -1, lastOpacity = -1;
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
				else if ((b->flags & 16) && eye[2] > 0) { statSmall++; out = true; }
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
				if (b->opacity != lastOpacity) { setupTexEnv(b->opacity); lastOpacity = b->opacity; }
				C3D_SetBufInfo(&c->bufs[i]);
				C3D_DrawElements(GPU_TRIANGLES, b->icount, C3D_UNSIGNED_SHORT, c->data + b->iofs);
				statDraws++;
				statTris += b->icount / 3;
			}
		}
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
enum { B_OFF, B_STATIC, B_WALK, B_DONE };
static int bPhase = B_OFF, bAngle = -1, bFrame;
static float bCpu, bCpuMax, bGpu, bDraws, bTris, bFrameMs;
static float startEye[3], startYaw;

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
	if (bPhase != B_WALK) return;
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
	bool end = wStage == 2 ? wDist >= routeLen() : wDist <= 0;
	if (end) {
		logw("  => %d frames, avg fps %.1f, missed frames (>34.5 ms) %d, stops (>50 ms) %d, worst %.1f ms, loads %d (setup max %.1f ms), load failures %d\n",
		     wFrames, 1000.0f * wFrames / wFrameSum, wOver34, wHitch, wWorst, wLoads, wSetupMax, loadFailed);
		logFlush();
		if (wStage == 2) benchStartWalk(3);
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

	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	dvlb = DVLB_ParseFile((u32*)world_shbin, world_shbin_size);
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	uMvp = shaderInstanceGetUniformLocation(prog.vertexShader, "mvp");
	uOrigin = shaderInstanceGetUniformLocation(prog.vertexShader, "origin");
	uFog = shaderInstanceGetUniformLocation(prog.vertexShader, "fogRow");
	AttrInfo_Init(&attr);
	AttrInfo_AddLoader(&attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(&attr, 1, GPU_SHORT, 2);
	AttrInfo_AddLoader(&attr, 2, GPU_UNSIGNED_BYTE, 4);

	printf("3dsOpenworld - Morrowind cell test\n");
	u64 t0 = ticks();
	if (!loadIndex() || !loadTextures()) {
		printf("\x1b[31mromfs data missing (tools/mwbake.py)\x1b[0m\nSTART to quit\n");
		while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
		goto done;
	}
	float texMs = msSince(t0);
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
			fclose(bf);
			logFile = fopen("sdmc:/3ds/mwcell3d/bench.log", "w");
			bPhase = B_STATIC;
			radius = 0; farDist = 7000.0f; autoFar = false; setCap(false);
		} else {
			logFile = fopen("sdmc:/3ds/mwcell3d/play.log", "a");
			setCap(true);
		}
		logw("--- %s 3DS, heap %lu KB, linear %lu KB, textures %d (%lu KB) in %.0f ms, %d cells baked, route %d points%s\n",
		     n3ds ? "New" : "Old", __ctru_heap_size / 1024, __ctru_linear_heap_size / 1024, ntex, texBytes / 1024, texMs,
		     navail, nroute, loaderThread ? "" : ", NO LOADER THREAD");
		logFlush();
		if (bPhase != B_OFF) printf("\x1b[33mBENCHMARK running (about 3 min),\nthis screen stays still meanwhile\x1b[0m\n");
	}
	startEye[0] = eye[0]; startEye[1] = eye[1]; startEye[2] = eye[2];
	startYaw = yaw;

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
		if (down & KEY_SELECT) {
			radius = (radius + 1) % 3;
			if (!autoFar) farDist = radius == 0 ? 7000.0f : radius == 1 ? 12000.0f : 18000.0f;
		}
		if (down & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			if (tp.px < 160) autoFar = !autoFar;
			else setCap(!cap30);
		}
		if (held & KEY_DUP) speed *= 1.0f + dt * 1.5f;
		if (held & KEY_DDOWN) speed /= 1.0f + dt * 1.5f;
		if (autoFar) {
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
		if (dt > 0) {   // smoothed velocity for the loader's prediction
			float k = dt * 3.0f; if (k > 1) k = 1;
			vel[0] += ((eye[0] - prevEye[0]) / dt - vel[0]) * k;
			vel[1] += ((eye[1] - prevEye[1]) / dt - vel[1]) * k;
		}
		prevEye[0] = eye[0]; prevEye[1] = eye[1];

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		u64 c0 = ticks();
		float setupMs = streamCells();   // after FrameBegin: the GPU is done with the last frame's buffers

		C3D_Mtx proj, view, mvp;
		Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 8.0f, farDist + 1000.0f, false);
		Mtx_LookAt(&view, FVec3_New(0, 0, 0), FVec3_New(fwd[0], fwd[1], fwd[2]), FVec3_New(0, 0, 1), false);
		Mtx_Multiply(&mvp, &proj, &view);

		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, SKY_R << 24 | SKY_G << 16 | SKY_B << 8 | 0xFF, 0);
		C3D_FrameDrawOn(top);
		drawWorld(&mvp, fwd);
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
				logw("fps %.1f cpu %.2f (max %.2f) gpu %.2f draws %d tris %lu | r%d far %.0f%s budget %.0f | cells %d missing %d | load read %.0f setup %.1f ms #%d fail %d | free %lu KB | cell %d,%d\n",
				     fps, cpuAvg, cpuMaxShown, gpuMs, statDraws, statTris, radius, farDist, autoFar ? " auto" : "", gpuBudget,
				     loaded, cellsMissing, lastIoMs, lastSetupMs, loadsTotal, loadFailed, linearSpaceFree() / 1024,
				     (int)floorf(eye[0] / CELL), (int)floorf(eye[1] / CELL));
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
			printf("\x1b[2mpad move  ABXY look  L/R down/up\nDpad U/D speed  L/R budget|far\nSELECT radius  START quit\ntouch left: auto/manual far\ntouch right: 30/60 fps\x1b[0m");
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
	shaderProgramFree(&prog);
	DVLB_Free(dvlb);
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
