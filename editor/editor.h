// The editor's state, shared by main.c and the tabs (tab_*.c).
#pragma once
#include "brush.h"
#include "cell.h"
#include "view.h"

typedef enum { TAB_TERRAIN, TAB_PAINT, TAB_OBJECTS, TAB_EMPTY, TAB_FILE, TAB_COUNT } Tab;

typedef struct {
	ECell cell;
	View view;
	Brush brush;
	Tool aTool;        // what A does (chosen on the touch screen); B, Y, X stay lower, smooth, flatten
	Tool tool;         // in use this frame
	Tab tab;
	float fps, brushMs, prepMs;
	char fileMsg[96];   // the last save / load result, shown on the "Plik" tab
} Editor;

// The world being edited: one plugin file the Construction Set and OpenMW-CS can open (editor/esp.h).
#define WORLD_DIR "sdmc:/3ds/mwedit/world"
#define WORLD_ESP WORLD_DIR "/world.esp"

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
void tabLater(Editor* e, const char* what);   // a tab still to come
