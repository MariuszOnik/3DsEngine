#include "landtex.h"

#include <dirent.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static LandTex* texs;
static int count;

int ltCount(void) { return count; }
const LandTex* ltGet(int i) { return i >= 0 && i < count ? &texs[i] : NULL; }

int ltFind(const char* file)
{
	for (int i = 0; i < count; i++)
		if (strcasecmp(texs[i].file, file) == 0) return i;
	return -1;
}

// ---- 64 x 64 from any size: each output pixel averages its share of the source (box filter)

static void shrink(const u8* src, int w, int h, u8* dst)
{
	for (int y = 0; y < LT_SIZE; y++) {
		int y0 = y * h / LT_SIZE, y1 = (y + 1) * h / LT_SIZE;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < LT_SIZE; x++) {
			int x0 = x * w / LT_SIZE, x1 = (x + 1) * w / LT_SIZE;
			if (x1 <= x0) x1 = x0 + 1;
			u32 s[3] = { 0, 0, 0 }, n = 0;
			for (int sy = y0; sy < y1; sy++)
				for (int sx = x0; sx < x1; sx++) {
					const u8* p = src + ((size_t)sy * w + sx) * 3;
					s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
					n++;
				}
			u8* o = dst + (y * LT_SIZE + x) * 3;
			for (int c = 0; c < 3; c++) o[c] = (u8)(s[c] / n);
		}
	}
}

static bool readPng(const char* path, u8* out64)
{
	png_image im;
	memset(&im, 0, sizeof im);
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&im, path)) return false;
	im.format = PNG_FORMAT_RGB;
	if (im.width == 0 || im.height == 0 || im.width > 2048 || im.height > 2048) { png_image_free(&im); return false; }
	u8* buf = malloc(PNG_IMAGE_SIZE(im));
	if (!buf) { png_image_free(&im); return false; }
	bool ok = png_image_finish_read(&im, NULL, buf, 0, NULL) != 0;
	if (ok) shrink(buf, im.width, im.height, out64);
	free(buf);
	return ok;
}

// ---- the touch screen's picture: RGB565 in the GPU's tiled order (8 x 8 tiles, Morton inside, the bottom row first)

static u32 morton(u32 x, u32 y)
{
	u32 m = 0;
	for (int b = 0; b < 3; b++) m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
	return m;
}

static void makeThumb(LandTex* t)
{
	if (!C3D_TexInit(&t->thumb, LT_SIZE, LT_SIZE, GPU_RGB565)) return;
	u16* d = t->thumb.data;
	for (int y = 0; y < LT_SIZE; y++)
		for (int x = 0; x < LT_SIZE; x++) {
			const u8* p = t->rgb + (y * LT_SIZE + x) * 3;
			int ty = LT_SIZE - 1 - y;
			d[((ty >> 3) * (LT_SIZE >> 3) + (x >> 3)) * 64 + morton(x & 7, ty & 7)] =
				(u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
		}
	C3D_TexFlush(&t->thumb);
	C3D_TexSetFilter(&t->thumb, GPU_LINEAR, GPU_LINEAR);
	t->sub = (Tex3DS_SubTexture){ LT_SIZE, LT_SIZE, 0.0f, 1.0f, 1.0f, 0.0f };
	t->img = (C2D_Image){ &t->thumb, &t->sub };
}

// ---- generated textures for an empty folder (tiling value noise)

static u32 hash2(int x, int y, int seed)
{
	u32 h = (u32)x * 374761393u + (u32)y * 668265263u + (u32)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static float noise(float x, float y, int period, int seed)
{
	int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = ((xi % period) + period) % period, y0 = ((yi % period) + period) % period;
	int x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
	float a = (hash2(x0, y0, seed) & 0xFFFF) / 65535.0f, b = (hash2(x1, y0, seed) & 0xFFFF) / 65535.0f;
	float c = (hash2(x0, y1, seed) & 0xFFFF) / 65535.0f, d = (hash2(x1, y1, seed) & 0xFFFF) / 65535.0f;
	float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
	return top + (bottom - top) * fy;
}

static void writeDefaults(void)
{
	static const struct { const char* file; u8 lo[3], hi[3]; int seed; } def[] = {
		{ "trawa.png", { 58, 80, 34 }, { 112, 138, 62 }, 1 },
		{ "ziemia.png", { 78, 60, 42 }, { 138, 112, 80 }, 2 },
		{ "piasek.png", { 168, 150, 112 }, { 214, 198, 156 }, 3 },
	};
	const int S = 256;
	u8* img = malloc(S * S * 3);
	if (!img) return;
	for (unsigned i = 0; i < sizeof def / sizeof def[0]; i++) {
		for (int y = 0; y < S; y++)
			for (int x = 0; x < S; x++) {
				float n = noise(x / 32.0f, y / 32.0f, S / 32, def[i].seed) * 0.5f +
				          noise(x / 8.0f, y / 8.0f, S / 8, def[i].seed) * 0.3f + noise(x / 2.0f, y / 2.0f, S / 2, def[i].seed) * 0.2f;
				for (int c = 0; c < 3; c++) img[(y * S + x) * 3 + c] = (u8)(def[i].lo[c] + (def[i].hi[c] - def[i].lo[c]) * n);
			}
		png_image im;
		memset(&im, 0, sizeof im);
		im.version = PNG_IMAGE_VERSION;
		im.width = S;
		im.height = S;
		im.format = PNG_FORMAT_RGB;
		char path[128];
		snprintf(path, sizeof path, LT_DIR "/%s", def[i].file);
		png_image_write_to_file(&im, path, 0, img, 0, NULL);
	}
	free(img);
}

// ---- the folder

static int byName(const void* a, const void* b) { return strcasecmp((const char*)a, (const char*)b); }

static int listPngs(char (*names)[48], int max)
{
	DIR* d = opendir(LT_DIR);
	if (!d) return 0;
	int n = 0;
	struct dirent* e;
	while ((e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		if (l < 5 || l >= 48 || strcasecmp(e->d_name + l - 4, ".png") != 0) continue;
		strcpy(names[n++], e->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof names[0], byName);
	return n;
}

int ltLoadAll(int* failed)
{
	static char names[LT_MAX][48];
	*failed = 0;
	mkdir("sdmc:/3ds/mwedit", 0777);
	mkdir(LT_DIR, 0777);
	int n = listPngs(names, LT_MAX);
	if (n == 0) {
		writeDefaults();
		n = listPngs(names, LT_MAX);
	}
	if (!texs) texs = calloc(LT_MAX, sizeof *texs);
	if (!texs) return 0;
	count = 0;
	for (int i = 0; i < n; i++) {
		LandTex* t = &texs[count];
		char path[128];
		snprintf(path, sizeof path, LT_DIR "/%s", names[i]);
		if (!readPng(path, t->rgb)) { (*failed)++; continue; }
		strcpy(t->file, names[i]);
		size_t l = strlen(names[i]) - 4;
		if (l >= sizeof t->name) l = sizeof t->name - 1;
		memcpy(t->name, names[i], l);
		t->name[l] = 0;
		makeThumb(t);
		count++;
	}
	return count;
}
