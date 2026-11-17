// The land textures to paint with: PNG files in sdmc:/3ds/mwedit/textures/ (any power-of-two square, 256 x 256
// suggested; seamless). Each is shrunk to 64 x 64: that many pixels of the cell image (1024 x 1024 over 8192 units)
// cover one texture repeat of 512 units, as Morrowind lays its land textures (one per VTEX square).
// An empty folder gets three generated textures (written there as PNG, so they can go to the PC too).
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#define LT_DIR "sdmc:/3ds/mwedit/textures"
#define LT_MAX 64
#define LT_SIZE 64

typedef struct {
	char name[40];              // the file name without .png (shown, and the LTEX id in the .esm)
	char file[48];              // the file name (the LTEX texture path: textures/<file> on the PC)
	u8 rgb[LT_SIZE * LT_SIZE * 3];   // row 0 = the top (north)
	C3D_Tex thumb;              // the same, for the touch screen
	Tex3DS_SubTexture sub;
	C2D_Image img;
} LandTex;

// Reads the folder (sorted by name). Returns how many; *failed gets the files that could not be read.
int ltLoadAll(int* failed);
int ltCount(void);
const LandTex* ltGet(int i);
int ltFind(const char* file);   // by file name (case does not matter), -1 if none
