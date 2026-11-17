// ETC1 encoder / decoder in the 3DS layout (see etc1.h).
#include "etc1.h"

#include <stdbool.h>
#include <string.h>

// The block layout in memory, chosen by tools/etc1test.c against tex3ds' output (bit 0: the image is walked
// bottom row first, bit 1: blocks within a tile are column-first, bit 2: big-endian blocks).
int etc1Layout = 1;

static const int kMods[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };

size_t etc1Size(int w, int h, int* levels)
{
	size_t total = 0;
	int n = 0;
	for (;;) {
		total += (size_t)w * h / 2;
		n++;
		if (w <= 8 || h <= 8) break;
		w /= 2; h /= 2;
	}
	if (levels) *levels = n;
	return total;
}

// Block (bx, by) of the image (4-pixel units, row 0 at the top) -> its index in memory.
static size_t blockIndex(int bx, int by, int w, int h)
{
	int gy = (etc1Layout & 1) ? (h / 4 - 1 - by) : by;   // row in the walk
	int tx = bx >> 1, ty = gy >> 1;
	int sx = bx & 1, sy = gy & 1;
	int sub = (etc1Layout & 2) ? (sx * 2 + sy) : (sy * 2 + sx);
	return ((size_t)ty * (w / 8) + tx) * 4 + sub;
}

static void putBlock(uint8_t* dst, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		dst[i] = (etc1Layout & 4) ? (uint8_t)(v >> (56 - 8 * i)) : (uint8_t)(v >> (8 * i));
}

static uint64_t getBlock(const uint8_t* src)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t)src[i] << ((etc1Layout & 4) ? (56 - 8 * i) : (8 * i));
	return v;
}

// The pixel (x, y) of a 4x4 block in the walk's orientation: when the walk starts at the bottom row, the block's
// own rows run upwards too.
static int rowInImage(int by, int y)
{
	return (etc1Layout & 1) ? by * 4 + 3 - y : by * 4 + y;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// One sub-block: base colour c[3] against its 8 pixels; the best table, its error and the pixel indices.
typedef struct { int table; int err; uint8_t idx[8]; } SubFit;

static SubFit fitSub(const int c[3], const uint8_t* const px[8])
{
	// err(m) = S + 2 m D + 3 m^2 per pixel (the modifier adds the same to the three channels; clamping ignored)
	int D[8], S[8];
	for (int i = 0; i < 8; i++) {
		int d0 = c[0] - px[i][0], d1 = c[1] - px[i][1], d2 = c[2] - px[i][2];
		D[i] = d0 + d1 + d2;
		S[i] = d0 * d0 + d1 * d1 + d2 * d2;
	}
	SubFit best = { 0, 0x7FFFFFFF, { 0 } };
	for (int t = 0; t < 8; t++) {
		const int a = kMods[t][0], b = kMods[t][1];
		const int m[4] = { a, b, -a, -b };
		int err = 0;
		uint8_t idx[8];
		for (int i = 0; i < 8; i++) {
			int be = 0x7FFFFFFF, bi = 0;
			for (int k = 0; k < 4; k++) {
				int e = 2 * m[k] * D[i] + 3 * m[k] * m[k];
				if (e < be) { be = e; bi = k; }
			}
			err += S[i] + be;
			idx[i] = (uint8_t)bi;
			if (err >= best.err) break;
		}
		if (err < best.err) { best.table = t; best.err = err; memcpy(best.idx, idx, 8); }
	}
	return best;
}

static uint64_t encodeBlock(const uint8_t* const p[4][4], int quality)
{
	uint64_t bestBits = 0;
	int bestErr = 0x7FFFFFFF;
	for (int flip = 0; flip < 2; flip++) {
		// sub-block s: its 8 pixels and their (x, y)
		const uint8_t* sp[2][8];
		int sxy[2][8][2];
		int n[2] = { 0, 0 };
		for (int x = 0; x < 4; x++)
			for (int y = 0; y < 4; y++) {
				int s = flip ? (y >= 2) : (x >= 2);
				sp[s][n[s]] = p[x][y];
				sxy[s][n[s]][0] = x; sxy[s][n[s]][1] = y;
				n[s]++;
			}
		int avg[2][3];
		for (int s = 0; s < 2; s++)
			for (int ch = 0; ch < 3; ch++) {
				int sum = 0;
				for (int i = 0; i < 8; i++) sum += sp[s][i][ch];
				avg[s][ch] = sum;   // x8
			}
		// the candidate modes: differential when the 5-bit colours are close enough, individual (4-bit) otherwise
		// or (quality 1) as well
		for (int mode = 0; mode < 2; mode++) {   // 0 differential, 1 individual
			int q[2][3], c[2][3];
			bool ok = true;
			for (int s = 0; s < 2; s++)
				for (int ch = 0; ch < 3; ch++) {
					if (mode == 0) {
						q[s][ch] = (avg[s][ch] * 31 + 8 * 255 / 2) / (8 * 255);
						c[s][ch] = (q[s][ch] << 3) | (q[s][ch] >> 2);
					} else {
						q[s][ch] = (avg[s][ch] * 15 + 8 * 255 / 2) / (8 * 255);
						c[s][ch] = q[s][ch] * 17;
					}
				}
			if (mode == 0)
				for (int ch = 0; ch < 3; ch++) {
					int d = q[1][ch] - q[0][ch];
					if (d < -4 || d > 3) ok = false;
				}
			if (!ok) continue;
			SubFit f0 = fitSub(c[0], sp[0]);
			SubFit f1 = fitSub(c[1], sp[1]);
			int err = f0.err + f1.err;
			if (err < bestErr) {
				bestErr = err;
				uint64_t v = 0;
				if (mode == 0) {
					for (int ch = 0; ch < 3; ch++) {
						int d = (q[1][ch] - q[0][ch]) & 7;
						v |= (uint64_t)q[0][ch] << (59 - 8 * ch);
						v |= (uint64_t)d << (56 - 8 * ch);
					}
					v |= 1ull << 33;
				} else {
					for (int ch = 0; ch < 3; ch++) {
						v |= (uint64_t)q[0][ch] << (60 - 8 * ch);
						v |= (uint64_t)q[1][ch] << (56 - 8 * ch);
					}
				}
				v |= (uint64_t)f0.table << 37;
				v |= (uint64_t)f1.table << 34;
				v |= (uint64_t)flip << 32;
				const SubFit* fs[2] = { &f0, &f1 };
				for (int s = 0; s < 2; s++)
					for (int i = 0; i < 8; i++) {
						int bit = sxy[s][i][0] * 4 + sxy[s][i][1];
						int id = fs[s]->idx[i];
						// 0 +a, 1 +b, 2 -a, 3 -b -> msb (negative), lsb (large)
						v |= (uint64_t)(id >> 1) << (16 + bit);
						v |= (uint64_t)(id & 1) << bit;
					}
				bestBits = v;
			}
			if (quality == 0 && mode == 0) break;   // fast: differential when it fits
		}
	}
	return bestBits;
}

void etc1Encode(const uint8_t* rgb, int w, int h, uint8_t* dst, int quality)
{
	for (int by = 0; by < h / 4; by++)
		for (int bx = 0; bx < w / 4; bx++) {
			const uint8_t* p[4][4];
			for (int y = 0; y < 4; y++) {
				const uint8_t* row = rgb + (size_t)rowInImage(by, y) * w * 3;
				for (int x = 0; x < 4; x++) p[x][y] = row + (bx * 4 + x) * 3;
			}
			putBlock(dst + blockIndex(bx, by, w, h) * 8, encodeBlock(p, quality));
		}
}

void etc1Decode(const uint8_t* src, int w, int h, uint8_t* rgb)
{
	for (int by = 0; by < h / 4; by++)
		for (int bx = 0; bx < w / 4; bx++) {
			uint64_t v = getBlock(src + blockIndex(bx, by, w, h) * 8);
			int c[2][3];
			if (v & (1ull << 33)) {
				for (int ch = 0; ch < 3; ch++) {
					int q0 = (int)(v >> (59 - 8 * ch)) & 31;
					int d = (int)(v >> (56 - 8 * ch)) & 7;
					if (d >= 4) d -= 8;
					int q1 = (q0 + d) & 31;
					c[0][ch] = (q0 << 3) | (q0 >> 2);
					c[1][ch] = (q1 << 3) | (q1 >> 2);
				}
			} else {
				for (int ch = 0; ch < 3; ch++) {
					c[0][ch] = ((int)(v >> (60 - 8 * ch)) & 15) * 17;
					c[1][ch] = ((int)(v >> (56 - 8 * ch)) & 15) * 17;
				}
			}
			int t[2] = { (int)(v >> 37) & 7, (int)(v >> 34) & 7 };
			int flip = (int)(v >> 32) & 1;
			for (int x = 0; x < 4; x++)
				for (int y = 0; y < 4; y++) {
					int s = flip ? (y >= 2) : (x >= 2);
					int bit = x * 4 + y;
					int id = (int)((v >> (16 + bit)) & 1) * 2 + (int)((v >> bit) & 1);
					int m = (id & 1) ? kMods[t[s]][1] : kMods[t[s]][0];
					if (id & 2) m = -m;
					uint8_t* o = rgb + ((size_t)rowInImage(by, y) * w + bx * 4 + x) * 3;
					for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)clamp255(c[s][ch] + m);
				}
		}
}

// Box filter to half size (dst may be src: it is read ahead of where it is written).
static void halve(const uint8_t* src, int w, int h, uint8_t* dst)
{
	int w2 = w / 2, h2 = h / 2;
	for (int y = 0; y < h2; y++)
		for (int x = 0; x < w2; x++) {
			const uint8_t* a = src + ((size_t)(2 * y) * w + 2 * x) * 3;
			const uint8_t* b = a + (size_t)w * 3;
			uint8_t* o = dst + ((size_t)y * w2 + x) * 3;
			for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)((a[ch] + a[3 + ch] + b[ch] + b[3 + ch] + 2) >> 2);
		}
}

void etc1EncodeMips(const uint8_t* rgb, int w, int h, uint8_t* dst, uint8_t* work, int quality)
{
	etc1Encode(rgb, w, h, dst, quality);
	dst += (size_t)w * h / 2;
	const uint8_t* cur = rgb;
	while (w > 8 && h > 8) {
		halve(cur, w, h, work);
		cur = work;
		w /= 2; h /= 2;
		etc1Encode(work, w, h, dst, quality);
		dst += (size_t)w * h / 2;
	}
}
