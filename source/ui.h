#pragma once
#include <stdbool.h>

#define UI_MAXITEMS 21
typedef struct { const char* icon; char label[32]; bool on; } UiItem;
typedef struct {
	UiItem item[UI_MAXITEMS];
	int nitems, selected;   // (on this page)
	int page, npages;
	const char* camName;
	bool torch;
	char status[64];
	bool combat, myTurn;            // the fight's window instead of the inventory
	int hpP, maxP, hpE, maxE;
	char enemy[48], msg[64];
	char note[96];                  // what A does to the object in reach / a script's message (over the item line)
} UiState;

enum { UI_NONE = -1, UI_CAMERA = 1000, UI_TORCH, UI_NPC_ADD, UI_NPC_CLEAR, UI_PREV, UI_NEXT, UI_ATK = 2000, UI_FLEE = 2003, UI_ITEM = 0 };

bool uiInit(void);                              // after C3D_Init; false: no atlas (still draws text)
void uiDraw(const UiState* s);                  // inside C3D_FrameBegin / End, after the 3D
// Over the 3D on the top screen: kind 1 Morrowind's crosshair at (x, y), 2 its mouse cursor (the tip
// at (x, y)); the label under it (the name of what it points at).
void uiDrawTop(C3D_RenderTarget* rt, int kind, float x, float y, const char* label, bool walking);
int uiTouch(const UiState* s, int px, int py);  // what a touch hit: UI_ITEM + i, UI_CAMERA.., UI_NONE
