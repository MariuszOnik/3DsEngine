// The editor's state, shared by main.c and the tabs (tab_*.c).
#pragma once
#include "brush.h"
#include "cell.h"
#include "view.h"

typedef enum { TAB_TERRAIN, TAB_PAINT, TAB_OBJECTS, TAB_VIEW, TAB_FILE, TAB_COUNT } Tab;

typedef struct {
	ECell cell;
	View view;
	Brush brush;
	Tool aTool;        // what A does (chosen on the touch screen); B, Y, X stay lower, smooth, flatten
	Tool tool;         // in use this frame
	Tab tab;
	float fps, brushMs, prepMs;
	char fileMsg[96];   // the last save / load result, shown on the "Plik" tab
	int paintTex, paintPage;   // the land texture A paints with (landtex.h), the page of the palette
	float paintRate;           // 0..1: how fast the texture covers the land under the brush's middle
	int ntex, texFailed;       // land textures read at the start, files that could not be read
	float ripples, waterDepth; // the "Widok" tab: the ripples' strength (0..1), units to the full depth colour
} Editor;

#define PAINT_PER_SEC 4.0f   // paintRate 1: full cover in a quarter of a second

// The world being edited: the game's own master file (like Morrowind.esm), opened by OpenMW-CS and the Construction
// Set as the game file (editor/esp.h). .esp plugins stay for mods built on it. world.esp: the first saves (17.11).
#define WORLD_DIR "sdmc:/3ds/mwedit/world"
#define WORLD_ESP WORLD_DIR "/world.esm"
#define WORLD_OLD WORLD_DIR "/world.esp"

// The "Plik" tab's actions (tab_file.c); fileLoad is also run at the start.
bool fileSave(Editor* e);
int fileLoad(Editor* e);

#define BRUSH_MIN 128.0f
#define BRUSH_MAX 4096.0f
#define RATE_MIN 100.0f
#define RATE_MAX 2400.0f

// The tabs: their widgets in the area between the tab row and the status bar.
void tabTerrain(Editor* e);
void tabFile(Editor* e);
void tabPaint(Editor* e);
void tabView(Editor* e);
void settingsLoad(Editor* e);   // sdmc:/3ds/mwedit/settings.txt (the "Widok" tab's values)
void tabLater(Editor* e, const char* what);   // a tab still to come
