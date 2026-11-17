// ETC1 for the 3DS GPU, on the console itself (terrain painting: the painted cell image is encoded at "Bake").
// Plain C, no 3DS headers: tools/etc1test.c builds it on the PC to check it against tex3ds.
//
// Images are RGB, 3 bytes a pixel, row 0 = the top of the image (as the baker's composite before it is flipped).
// The 3DS layout (as tex3ds writes it for the baker, mwbake.encode_etc): 8x8 tiles, four 4x4 ETC1 blocks per tile,
// each block a little-endian 64-bit word; all mipmap levels one after the other, down to 8x8.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Bytes of all mipmap levels of a w x h ETC1 texture (w, h powers of two >= 8), and the level count.
size_t etc1Size(int w, int h, int* levels);

// One level: rgb (w*h*3) -> dst (w*h/2 bytes). quality 0: fast (the base colour from the average only),
// 1: also tries the neighbouring base colours (slower, a little better).
void etc1Encode(const uint8_t* rgb, int w, int h, uint8_t* dst, int quality);

// All levels, box-filtered like tex3ds -m box. work: w*h*3/4 bytes of scratch for the smaller levels.
void etc1EncodeMips(const uint8_t* rgb, int w, int h, uint8_t* dst, uint8_t* work, int quality);

// Painting: only what a brush touched. rgb is the whole level-0 image (already painted), [x0, x1) x [y0, y1) the
// changed pixels; the blocks covering them are encoded again in every level of dst (the whole mip chain, as
// etc1EncodeMips writes it). Smaller levels average 2^k x 2^k pixels of level 0 (close to the box chain).
// quality 0 for the live preview, 1 for the bake.
void etc1UpdateRect(const uint8_t* rgb, int w, int h, int x0, int y0, int x1, int y1, uint8_t* dst, int quality);

// One level back to rgb (w*h*3).
void etc1Decode(const uint8_t* src, int w, int h, uint8_t* rgb);
