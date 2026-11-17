// PC check of source/etc1.c against the baker's terrain textures (tex3ds output):
//   gcc -O2 -o etc1test tools/etc1test.c source/etc1.c && ./etc1test romfs/cells/-2_-9.bin [out_prefix]
// 1. finds the block layout: decodes the cell's ETC1 level 0 with each of the 8 layouts, the smoothest image wins;
// 2. re-encodes that image (quality 0 and 1) and times it, PSNR against the decoded tex3ds image;
// 3. writes <out_prefix>_src.ppm and _q0.ppm to look at.
#include "../source/etc1.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int etc1Layout;

static double now(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double roughness(const uint8_t* rgb, int w, int h)
{
	double s = 0;
	for (int y = 0; y < h - 1; y++)
		for (int x = 0; x < w - 1; x++)
			for (int c = 0; c < 3; c++) {
				int p = rgb[(y * w + x) * 3 + c];
				s += abs(p - rgb[(y * w + x + 1) * 3 + c]) + abs(p - rgb[((y + 1) * w + x) * 3 + c]);
			}
	return s / ((double)w * h);
}

static double psnr(const uint8_t* a, const uint8_t* b, size_t n)
{
	double e = 0;
	for (size_t i = 0; i < n; i++) { double d = (double)a[i] - b[i]; e += d * d; }
	e /= n;
	return e == 0 ? 99 : 10 * log10(255.0 * 255.0 / e);
}

static void ppm(const char* path, const uint8_t* rgb, int w, int h)
{
	FILE* f = fopen(path, "wb");
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb, 1, (size_t)w * h * 3, f);
	fclose(f);
}

// etc1test --ppm image.ppm: encodes a raw image (the baker's composite) and reports PSNR and time.
static int ppmMode(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	int w, h, mx;
	if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3) return 1;
	fgetc(f);
	size_t px = (size_t)w * h * 3;
	uint8_t* img = malloc(px); uint8_t* img2 = malloc(px); uint8_t* work = malloc(px / 4);
	fread(img, 1, px, f); fclose(f);
	int levels; size_t total = etc1Size(w, h, &levels);
	uint8_t* enc = malloc(total);
	for (int q = 0; q < 2; q++) {
		double t0 = now();
		etc1EncodeMips(img, w, h, enc, work, q);
		double t1 = now();
		etc1Decode(enc, w, h, img2);
		double mad = 0;
		for (size_t i = 0; i < px; i++) mad += abs((int)img[i] - img2[i]);
		printf("quality %d: %.0f ms, PSNR %.2f dB, mean abs diff %.2f (against the source)\n", q, (t1 - t0) * 1000,
			psnr(img, img2, px), mad / px);
	}
	return 0;
}

int main(int argc, char** argv)
{
	if (argc > 2 && !strcmp(argv[1], "--ppm")) return ppmMode(argv[2]);
	if (argc < 2) { fprintf(stderr, "usage: etc1test cell.bin [out_prefix] | --ppm image.ppm\n"); return 1; }
	FILE* f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t* buf = malloc(n);
	fread(buf, 1, n, f); fclose(f);
	uint16_t tsize; uint32_t ttofs;
	memcpy(&tsize, buf + 14, 2); memcpy(&ttofs, buf + 16, 4);
	int ts = tsize & 0xFFF, fmt = tsize >> 12;
	printf("%s: terrain %dx%d format %d at %u\n", argv[1], ts, ts, fmt, ttofs);
	if (fmt != 2) { fprintf(stderr, "not ETC1\n"); return 1; }
	const uint8_t* src = buf + ttofs;
	size_t px = (size_t)ts * ts * 3;
	uint8_t* img = malloc(px); uint8_t* img2 = malloc(px);
	int best = -1; double bestR = 1e30;
	for (int l = 0; l < 8; l++) {
		etc1Layout = l;
		etc1Decode(src, ts, ts, img);
		double r = roughness(img, ts, ts);
		printf("  layout %d: roughness %.2f\n", l, r);
		if (r < bestR) { bestR = r; best = l; }
	}
	etc1Layout = best;
	printf("layout %d\n", best);
	etc1Decode(src, ts, ts, img);
	const char* pre = argc > 2 ? argv[2] : "etc1";
	char path[512];
	snprintf(path, sizeof path, "%s_src.ppm", pre); ppm(path, img, ts, ts);

	int levels; size_t total = etc1Size(ts, ts, &levels);
	uint8_t* enc = malloc(total); uint8_t* work = malloc(px / 4);
	for (int q = 0; q < 2; q++) {
		double t0 = now();
		etc1EncodeMips(img, ts, ts, enc, work, q);
		double t1 = now();
		etc1Decode(enc, ts, ts, img2);
		printf("quality %d: %d levels, %zu bytes, %.0f ms on this PC, PSNR %.2f dB vs tex3ds' decoded image\n", q,
			levels, total, (t1 - t0) * 1000, psnr(img, img2, px));
		if (q == 0) { snprintf(path, sizeof path, "%s_q0.ppm", pre); ppm(path, img2, ts, ts); }
	}
	// round trip of the layout: my encoder's level 0 against the baked level 0 layout, byte for byte would need the
	// same encoder; instead: decode(encode(x)) must look like x (PSNR above) and the mip chain must be in place.
	etc1Decode(enc + (size_t)ts * ts / 2, ts / 2, ts / 2, img2);
	printf("level 1 roughness %.2f (sane if close to level 0's %.2f)\n", roughness(img2, ts / 2, ts / 2), bestR);
	return 0;
}
