// Dynamic objects: loading objects.bin, their state, the one in reach, door swings. Drawn by main.c (drawObjects).
#include "objects.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WObj* objs;
int nobj;
const ObjBase* objBases;
const ObjModel* objModels;
const ObjPart* objParts;
u8* objData;
int objNModels, objNParts;
u32 objBytes;
static int nbase;
static const char* strs;
static u32 nstrs;

typedef struct { char magic[4]; u32 nbase, nmodel, npart, nobj, baseofs, modelofs, partofs, objofs, strofs, strbytes; } __attribute__((packed)) ObjHdr;

bool objLoad(void)
{
	FILE* f = fopen("romfs:/objects.bin", "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	objData = (u8*)linearAlloc(n);
	bool ok = objData && fread(objData, 1, n, f) == (size_t)n;
	fclose(f);
	const ObjHdr* h = (const ObjHdr*)objData;
	if (!ok || n < (long)sizeof *h || memcmp(h->magic, "MWO3", 4) || h->strofs + h->strbytes > (u32)n) {
		if (objData) linearFree(objData);
		objData = NULL;
		return false;
	}
	objBytes = n;
	nbase = h->nbase; objNModels = h->nmodel; objNParts = h->npart;
	objBases = (const ObjBase*)(objData + h->baseofs);
	objModels = (const ObjModel*)(objData + h->modelofs);
	objParts = (const ObjPart*)(objData + h->partofs);
	strs = (const char*)(objData + h->strofs); nstrs = h->strbytes;
	nobj = h->nobj;
	objs = (WObj*)calloc(nobj ? nobj : 1, sizeof(WObj));
	const ObjRec* r = (const ObjRec*)(objData + h->objofs);
	for (int i = 0; i < nobj; i++) {
		WObj* o = &objs[i];
		o->frmr = r[i].frmr; o->plugin = r[i].plugin; o->base = r[i].base < nbase ? r[i].base : 0;
		o->gx = r[i].gx; o->gy = r[i].gy;
		memcpy(o->pos, r[i].pos, 12); memcpy(o->rot, r[i].rot, 12); o->scale = r[i].scale;
		o->destOfs = r[i].destOfs;
		memcpy(o->dest, r[i].dest, 16);
	}
	GSPGPU_FlushDataCache(objData, n);
	return true;
}

static const char* str(u32 ofs) { return ofs < nstrs ? strs + ofs : ""; }
const char* objId(int i) { return i >= 0 && i < nobj ? str(objBases[objs[i].base].idOfs) : ""; }
const char* objName(int i) { return i >= 0 && i < nobj ? str(objBases[objs[i].base].nameOfs) : ""; }
int objType(int i) { return i >= 0 && i < nobj ? objBases[objs[i].base].type : 0; }
const char* objDest(int i) { return i >= 0 && i < nobj && objs[i].destOfs != 0xFFFFFFFFu ? str(objs[i].destOfs) : NULL; }
const ObjModel* objModel(int i)
{
	int m = objBases[objs[i].base].model;
	return m < objNModels ? &objModels[m] : NULL;
}

int objFind(const char* id, int after)
{
	for (int i = after + 1; i < nobj; i++)
		if (!strcmp(objId(i), id)) return i;
	return -1;
}

int objNearest(const float feet[3], float yaw, float reach)
{
	float fx = sinf(yaw), fy = cosf(yaw);
	int best = -1;
	float bestScore = 1e30f;
	for (int i = 0; i < nobj; i++) {
		WObj* o = &objs[i];
		if (o->flags & OF_TAKEN) continue;
		const ObjModel* m = objModel(i);
		float r = m ? m->radius * o->scale : 0;
		float dx = o->pos[0] - feet[0], dy = o->pos[1] - feet[1], dz = o->pos[2] - feet[2];
		if (dz < -120.0f || dz > 200.0f) continue;             // another floor
		float d = sqrtf(dx * dx + dy * dy);
		// a big thing (a door, a chest) is reached at its edge, not its centre
		float edge = d - (r < 80.0f ? r * 0.5f : 40.0f);
		if (edge > reach) continue;
		float along = d > 1.0f ? (dx * fx + dy * fy) / d : 1.0f;   // 1 straight ahead, -1 behind
		if (along < -0.2f && edge > 30.0f) continue;
		float score = edge - along * 40.0f;
		if (score < bestScore) { bestScore = score; best = i; }
	}
	return best;
}

void objUpdate(float dt)
{
	for (int i = 0; i < nobj; i++) {
		WObj* o = &objs[i];
		float want = (o->flags & OF_OPEN) && objType(i) == OT_DOOR ? 1.0f : 0.0f;
		if (o->open < want) { o->open += dt * 1.6f; if (o->open > want) o->open = want; }
		else if (o->open > want) { o->open -= dt * 1.6f; if (o->open < want) o->open = want; }
	}
}

static void mul3(float* out, const float* a, const float* b)
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

void objMatrix(int i, const float eye[3], float out[12])
{
	const WObj* o = &objs[i];
	const ObjModel* m = objModel(i);
	// as the baker: world = X(-rx) Y(-ry) Z(-rz) v (+ a door's swing round its own up axis, at its hinge = origin)
	float a = -o->rot[0], b = -o->rot[1], c = -o->rot[2] - o->open * 1.5708f;
	float X[9] = { 1, 0, 0, 0, cosf(a), -sinf(a), 0, sinf(a), cosf(a) };
	float Y[9] = { cosf(b), 0, sinf(b), 0, 1, 0, -sinf(b), 0, cosf(b) };
	float Z[9] = { cosf(c), -sinf(c), 0, sinf(c), cosf(c), 0, 0, 0, 1 };
	float XY[9], R[9];
	mul3(XY, X, Y);
	mul3(R, XY, Z);
	float s = o->scale * (m ? m->quant : 1.0f);
	for (int r = 0; r < 3; r++) {
		out[r * 4] = R[r * 3] * s; out[r * 4 + 1] = R[r * 3 + 1] * s; out[r * 4 + 2] = R[r * 3 + 2] * s;
		out[r * 4 + 3] = o->pos[r] - eye[r];
	}
}
