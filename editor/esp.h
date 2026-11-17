// Morrowind / OpenMW-CS plugin files (.esp, TES3 format): the editor's terrain saved where the Construction Set and
// OpenMW-CS can open it. Plain C + stdio, no 3DS calls (tested on the PC too).
//
// Saving puts one exterior cell's LAND (VNML, VHGT, WNAM) into the file: it replaces that cell's LAND if the file has
// one, adds an empty CELL if it has none, and keeps every other record as it is (references, records made in
// OpenMW-CS). The file is created when it does not exist yet.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define ESP_GRID 65   // heights per cell side, every 128 units; row 0 = the south edge

// Writes heights h[ESP_GRID * ESP_GRID] of the exterior cell (gx, gy). msg (may be NULL) gets a line for the user.
bool espSaveLand(const char* path, int gx, int gy, const float* h, char* msg, size_t msgSize);

// Reads the heights of (gx, gy) into h: 1 found, 0 the file has no such LAND, -1 no file or not a TES3 file.
int espLoadLand(const char* path, int gx, int gy, float* h);

// How many LAND records the file has (-1: no file / not TES3).
int espCountLand(const char* path);
