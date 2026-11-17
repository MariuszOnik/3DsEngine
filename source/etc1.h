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

// One level back to rgb (w*h*3).
void etc1Decode(const uint8_t* src, int w, int h, uint8_t* rgb);
