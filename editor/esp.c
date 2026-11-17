#include "esp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A TES3 record: name[4], size (of the subrecords), 4 unused bytes, flags; then subrecords: name[4], size, data.
#define REC_HEAD 16
#define SUB_HEAD 8
#define VHGT_SIZE (4 + ESP_GRID * ESP_GRID + 3)
#define HEIGHT_SCALE 8.0f   // VHGT stores heights in steps of 8 units

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// ---- a growing output buffer

typedef struct { uint8_t* d; size_t n, cap; bool bad; } Buf;

static uint8_t* bufGrow(Buf* b, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 65536;
		while (cap < b->n + n) cap *= 2;
		uint8_t* d = realloc(b->d, cap);
		if (!d) { b->bad = true; return NULL; }
		b->d = d;
		b->cap = cap;
	}
	uint8_t* p = b->d + b->n;
	b->n += n;
	return p;
}

static void bufPut(Buf* b, const void* p, size_t n)
{
	uint8_t* o = bufGrow(b, n);
	if (o) memcpy(o, p, n);
}

static size_t recBegin(Buf* b, const char* name)
{
	size_t at = b->n;
	uint8_t* o = bufGrow(b, REC_HEAD);
	if (o) { memcpy(o, name, 4); memset(o + 4, 0, 12); }
	return at;
}

static void recEnd(Buf* b, size_t at)
{
	if (!b->bad) wr32(b->d + at + 4, (uint32_t)(b->n - at - REC_HEAD));
}

static uint8_t* sub(Buf* b, const char* name, size_t size)
{
	uint8_t* o = bufGrow(b, SUB_HEAD + size);
	if (!o) return NULL;
	memcpy(o, name, 4);
	wr32(o + 4, (uint32_t)size);
	memset(o + SUB_HEAD, 0, size);
	return o + SUB_HEAD;
}

// ---- reading a whole file

static uint8_t* readFile(const char* path, size_t* size)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* d = n > 0 ? malloc((size_t)n) : NULL;
	if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
	fclose(f);
	*size = d ? (size_t)n : 0;
	return d;
}

static bool isTes3(const uint8_t* d, size_t n) { return n >= REC_HEAD && memcmp(d, "TES3", 4) == 0; }

// The subrecord `name` inside the record at r (NULL if none); *size gets its size.
static const uint8_t* findSub(const uint8_t* r, const char* name, uint32_t* size)
{
	uint32_t len = rd32(r + 4);
	const uint8_t* p = r + REC_HEAD;
	const uint8_t* end = p + len;
	while (p + SUB_HEAD <= end) {
		uint32_t s = rd32(p + 4);
		if (p + SUB_HEAD + s > end) break;
		if (memcmp(p, name, 4) == 0) { *size = s; return p + SUB_HEAD; }
		p += SUB_HEAD + s;
	}
	return NULL;
}

static bool landIs(const uint8_t* r, int gx, int gy)
{
	uint32_t s;
	const uint8_t* intv = findSub(r, "INTV", &s);
	return intv && s >= 8 && (int32_t)rd32(intv) == gx && (int32_t)rd32(intv + 4) == gy;
}

static bool exteriorCellIs(const uint8_t* r, int gx, int gy)
{
	uint32_t s;
	const uint8_t* data = findSub(r, "DATA", &s);
	return data && s >= 12 && !(rd32(data) & 1) && (int32_t)rd32(data + 4) == gx && (int32_t)rd32(data + 8) == gy;
}

// ---- the records we write

static void writeHeader(Buf* b)
{
	size_t at = recBegin(b, "TES3");
	uint8_t* h = sub(b, "HEDR", 300);
	if (h) {
		float version = 1.3f;
		memcpy(h, &version, 4);
		// flags 0: a plugin; author[32], description[256], then the record count (set when the file is done)
		strcpy((char*)h + 8, "mwedit");
		strcpy((char*)h + 40, "3dsOpenworld: teren z edytora na 3DS");
	}
	recEnd(b, at);
}

static void writeCell(Buf* b, int gx, int gy)
{
	size_t at = recBegin(b, "CELL");
	sub(b, "NAME", 1);   // exterior cells have an empty name
	uint8_t* d = sub(b, "DATA", 12);
	if (d) { wr32(d, 0); wr32(d + 4, (uint32_t)gx); wr32(d + 8, (uint32_t)gy); }
	recEnd(b, at);
}

static int8_t clampS8(float v) { return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : lroundf(v)); }

// ---- land textures (LTEX: NAME id, INTV index, DATA texture file; VTEX holds index + 1)

#define LTEX_MAX 512
typedef struct { int n, maxIndex; int index[LTEX_MAX]; char file[LTEX_MAX][64]; } LtexTable;

static int strieq(const char* a, const char* b)
{
	for (; *a && *b; a++, b++) {
		int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
		if (x != y) return 0;
	}
	return *a == *b;
}

static void ltexAdd(LtexTable* t, int index, const char* file, size_t len)
{
	if (t->n >= LTEX_MAX) return;
	if (len > 63) len = 63;
	memcpy(t->file[t->n], file, len);
	t->file[t->n][len] = 0;
	t->index[t->n++] = index;
	if (index > t->maxIndex) t->maxIndex = index;
}

static void ltexRead(LtexTable* t, const uint8_t* r)
{
	uint32_t si, sd;
	const uint8_t* intv = findSub(r, "INTV", &si);
	const uint8_t* data = findSub(r, "DATA", &sd);
	if (!intv || si < 4 || !data) return;
	size_t len = strnlen((const char*)data, sd);
	ltexAdd(t, (int)rd32(intv), (const char*)data, len);
}

static int ltexFind(const LtexTable* t, const char* file)
{
	for (int i = 0; i < t->n; i++)
		if (strieq(t->file[i], file)) return t->index[i];
	return -1;
}

static const char* ltexFile(const LtexTable* t, int index)
{
	for (int i = 0; i < t->n; i++)
		if (t->index[i] == index) return t->file[i];
	return NULL;
}

static void writeLtex(Buf* b, int index, const char* file)
{
	size_t at = recBegin(b, "LTEX");
	size_t l = strlen(file);
	const char* dot = strrchr(file, '.');
	size_t idl = dot ? (size_t)(dot - file) : l;
	uint8_t* p = sub(b, "NAME", idl + 1);
	if (p) memcpy(p, file, idl);
	p = sub(b, "INTV", 4);
	if (p) wr32(p, (uint32_t)index);
	p = sub(b, "DATA", l + 1);
	if (p) memcpy(p, file, l);
	recEnd(b, at);
}

// The file's VTEX order: 4 x 4 groups of 4 x 4 squares
static int vtexAt(int i)
{
	int y1 = i >> 6, x1 = (i >> 4) & 3, y2 = (i >> 2) & 3, x2 = i & 3;
	return (y1 * 4 + y2) * 16 + x1 * 4 + x2;
}

static void writeLand(Buf* b, int gx, int gy, const float* h, const uint16_t* vtexFile)
{
	const int G = ESP_GRID;
	size_t at = recBegin(b, "LAND");
	uint8_t* p = sub(b, "INTV", 8);
	if (p) { wr32(p, (uint32_t)gx); wr32(p + 4, (uint32_t)gy); }
	p = sub(b, "DATA", 4);
	if (p) wr32(p, vtexFile ? 1 | 4 : 1);   // heights and normals, textures (no vertex colours)

	// normals, as the editor lights the land (central differences, 128 units apart)
	p = sub(b, "VNML", G * G * 3);
	for (int y = 0; p && y < G; y++)
		for (int x = 0; x < G; x++) {
			int xl = x > 0 ? x - 1 : x, xr = x < G - 1 ? x + 1 : x;
			int yd = y > 0 ? y - 1 : y, yu = y < G - 1 ? y + 1 : y;
			float nx = -(h[y * G + xr] - h[y * G + xl]) / ((xr - xl) * 128.0f);
			float ny = -(h[yu * G + x] - h[yd * G + x]) / ((yu - yd) * 128.0f);
			float l = sqrtf(nx * nx + ny * ny + 1);
			int8_t* n = (int8_t*)p + (y * G + x) * 3;
			n[0] = clampS8(nx / l * 127);
			n[1] = clampS8(ny / l * 127);
			n[2] = clampS8(1 / l * 127);
		}

	// heights: steps of 8 as differences (the first of a row from the row below's first, the rest from the left
	// neighbour). A slope steeper than 127 steps per 128 units is cut; the error is carried, not added up.
	p = sub(b, "VHGT", VHGT_SIZE);
	if (p) {
		float base = roundf(h[0] / HEIGHT_SCALE);
		memcpy(p, &base, 4);
		int8_t* d = (int8_t*)p + 4;
		float rowStart = base;
		for (int y = 0; y < G; y++) {
			int8_t s = clampS8(roundf(h[y * G] / HEIGHT_SCALE) - rowStart);
			d[y * G] = s;
			rowStart += s;
			float v = rowStart;
			for (int x = 1; x < G; x++) {
				s = clampS8(roundf(h[y * G + x] / HEIGHT_SCALE) - v);
				d[y * G + x] = s;
				v += s;
			}
		}
	}

	// the world map's 9 x 9 heights (every 8th point, in steps of 128 units)
	p = sub(b, "WNAM", 81);
	for (int y = 0; p && y < 9; y++)
		for (int x = 0; x < 9; x++) ((int8_t*)p)[y * 9 + x] = clampS8(h[(y * 8) * G + x * 8] / 128.0f);

	if (vtexFile) {
		p = sub(b, "VTEX", 512);
		for (int i = 0; p && i < 256; i++) {
			uint16_t v = vtexFile[vtexAt(i)];
			p[i * 2] = (uint8_t)v;
			p[i * 2 + 1] = (uint8_t)(v >> 8);
		}
	}
	recEnd(b, at);
}


// ---- the calls

static bool isRec(const uint8_t* r, const char* name) { return memcmp(r, name, 4) == 0; }

bool espSaveLand(const char* path, int gx, int gy, const float* h, const uint16_t* vtex, EspTexFile texFile,
                 char* msg, size_t msgSize)
{
	char dummy[8];
	if (!msg) { msg = dummy; msgSize = sizeof dummy; }
	size_t n = 0;
	uint8_t* old = readFile(path, &n);
	if (old && !isTes3(old, n)) {
		snprintf(msg, msgSize, "To nie jest plik .esm / .esp");
		free(old);
		return false;
	}

	// the land textures the file has already (by file name), and the ones this cell needs added
	static LtexTable lt;
	memset(&lt, 0, sizeof lt);
	lt.maxIndex = -1;
	for (size_t at = 0; old && at + REC_HEAD <= n;) {
		size_t len = REC_HEAD + rd32(old + at + 4);
		if (at + len > n) break;
		if (isRec(old + at, "LTEX")) ltexRead(&lt, old + at);
		at += len;
	}
	static uint16_t vt[256];
	int known = lt.n, added[256], nadded = 0;
	for (int i = 0; vtex && i < 256; i++) {
		const char* file = vtex[i] && texFile ? texFile(vtex[i] - 1) : NULL;
		int li = file ? ltexFind(&lt, file) : -1;
		if (file && li < 0) {
			li = lt.maxIndex + 1;
			ltexAdd(&lt, li, file, strlen(file));
			added[nadded++] = lt.n - 1;
		}
		vt[i] = (uint16_t)(li >= 0 ? li + 1 : 0);
	}
	(void)known;
	const uint16_t* vtexFile = vtex ? vt : NULL;

	Buf b = { 0 };
	int records = 0, lands = 0;
	bool landDone = false, cellFound = false;
	if (old) {
		size_t at = 0;
		while (at + REC_HEAD <= n) {
			const uint8_t* r = old + at;
			size_t len = REC_HEAD + rd32(r + 4);
			if (at + len > n) break;   // a cut-off end: dropped
			if (at == 0) bufPut(&b, r, len);   // the header as it was (masters, author); its count is set below
			else if (isRec(r, "LAND") && landIs(r, gx, gy)) {
				if (!landDone) { writeLand(&b, gx, gy, h, vtexFile); landDone = true; records++; lands++; }
			} else {
				if (isRec(r, "CELL") && exteriorCellIs(r, gx, gy)) cellFound = true;
				if (isRec(r, "LAND")) lands++;
				bufPut(&b, r, len);
				records++;
			}
			if (at == 0)   // new textures right after the header: before any LAND that uses them
				for (int i = 0; i < nadded; i++) { writeLtex(&b, lt.index[added[i]], lt.file[added[i]]); records++; }
			at += len;
		}
		free(old);
	} else {
		writeHeader(&b);
		for (int i = 0; i < nadded; i++) { writeLtex(&b, lt.index[added[i]], lt.file[added[i]]); records++; }
	}
	if (!cellFound) { writeCell(&b, gx, gy); records++; }
	if (!landDone) { writeLand(&b, gx, gy, h, vtexFile); records++; lands++; }

	// the record count in HEDR; a .esm gets the master flag (a game file others build on), a .esp loses it
	uint32_t hs;
	uint8_t* hedr = b.bad ? NULL : (uint8_t*)findSub(b.d, "HEDR", &hs);
	if (hedr && hs >= 300) {
		size_t pl = strlen(path);
		bool master = pl >= 4 && (strcmp(path + pl - 4, ".esm") == 0 || strcmp(path + pl - 4, ".ESM") == 0);
		wr32(hedr + 4, master ? (rd32(hedr + 4) | 1) : (rd32(hedr + 4) & ~1u));
		wr32(hedr + 296, (uint32_t)records);
	}

	bool ok = !b.bad;
	if (ok) {   // into a new file first: a failed write must not eat the old one
		char tmp[512];
		snprintf(tmp, sizeof tmp, "%s.tmp", path);
		FILE* f = fopen(tmp, "wb");
		ok = f && fwrite(b.d, 1, b.n, f) == b.n;
		if (f && fclose(f) != 0) ok = false;
		if (ok) {
			remove(path);
			ok = rename(tmp, path) == 0;
		} else
			remove(tmp);
	}
	if (ok)
		snprintf(msg, msgSize, "Zapisano: %d %s, %u KB", lands, lands == 1 ? "komórka" : lands < 5 ? "komórki" : "komórek",
		         (unsigned)((b.n + 1023) / 1024));
	else
		snprintf(msg, msgSize, b.bad ? "Za mało pamięci" : "Nie udało się zapisać pliku");
	free(b.d);
	return ok;
}

int espLoadLand(const char* path, int gx, int gy, float* h, uint16_t* vtex, EspTexIndex texIndex)
{
	size_t n;
	uint8_t* d = readFile(path, &n);
	if (!d) return -1;
	if (!isTes3(d, n)) { free(d); return -1; }
	static LtexTable lt;
	memset(&lt, 0, sizeof lt);
	const uint8_t* land = NULL;
	for (size_t at = 0; at + REC_HEAD <= n;) {
		size_t len = REC_HEAD + rd32(d + at + 4);
		if (at + len > n) break;
		if (isRec(d + at, "LTEX")) ltexRead(&lt, d + at);
		else if (!land && isRec(d + at, "LAND") && landIs(d + at, gx, gy)) land = d + at;
		at += len;
	}
	uint32_t s;
	const uint8_t* v = land ? findSub(land, "VHGT", &s) : NULL;
	if (!v || s < 4 + ESP_GRID * ESP_GRID) { free(d); return 0; }

	const int G = ESP_GRID;
	float rowStart;
	memcpy(&rowStart, v, 4);
	const int8_t* dh = (const int8_t*)v + 4;
	for (int y = 0; y < G; y++) {
		rowStart += dh[y * G];
		float hh = rowStart;
		h[y * G] = hh * HEIGHT_SCALE;
		for (int x = 1; x < G; x++) {
			hh += dh[y * G + x];
			h[y * G + x] = hh * HEIGHT_SCALE;
		}
	}

	if (vtex) {
		memset(vtex, 0, 256 * sizeof *vtex);
		const uint8_t* t = findSub(land, "VTEX", &s);
		for (int i = 0; t && s >= 512 && i < 256; i++) {
			int li = (t[i * 2] | t[i * 2 + 1] << 8) - 1;
			const char* file = li >= 0 ? ltexFile(&lt, li) : NULL;
			int k = file && texIndex ? texIndex(file) : -1;
			vtex[vtexAt(i)] = (uint16_t)(k >= 0 ? k + 1 : 0);
		}
	}
	free(d);
	return 1;
}

int espCountLand(const char* path)
{
	size_t n;
	uint8_t* d = readFile(path, &n);
	if (!d) return -1;
	int count = isTes3(d, n) ? 0 : -1;
	for (size_t at = 0; count >= 0 && at + REC_HEAD <= n;) {
		size_t len = REC_HEAD + rd32(d + at + 4);
		if (at + len > n) break;
		if (isRec(d + at, "LAND")) count++;
		at += len;
	}
	free(d);
	return count;
}
