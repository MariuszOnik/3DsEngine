// The bottom screen: a Morrowind window (its own frame textures and item icons, tools/uibake.py)
// with the inventory as icons to touch, a line about the touched item, the frame's cost and a row
// of buttons (camera, torch, NPCs). Drawn with citro2d, only when something changed (and twice a
// second for the numbers): the bottom screen keeps its last picture in between.

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

#define MAXP 64
static C2D_SpriteSheet sheet;
static char names[MAXP][32];
static int npieces;
static C3D_RenderTarget* bot;
static C2D_TextBuf tbuf;
static bool ready;

#define GOLD C2D_Color32(0xCA, 0xA5, 0x60, 0xFF)
#define PALE C2D_Color32(0xDF, 0xC9, 0x9F, 0xFF)
#define DIM C2D_Color32(0x93, 0x7A, 0x4E, 0xFF)

static int piece(const char* n)
{
	for (int i = 0; i < npieces; i++) if (!strcmp(names[i], n)) return i;
	return -1;
}

bool uiInit(void)
{
	sheet = C2D_SpriteSheetLoad("romfs:/ui.t3x");
	FILE* f = fopen("romfs:/ui.txt", "r");
	if (f) {
		while (npieces < MAXP && fgets(names[npieces], sizeof names[0], f)) {
			names[npieces][strcspn(names[npieces], "\r\n")] = 0;
			npieces++;
		}
		fclose(f);
	}
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tbuf = C2D_TextBufNew(1024);
	ready = bot != NULL;
	return ready && sheet != NULL;
}

static void img(int p, float x, float y, float w, float h)
{
	if (!sheet || p < 0 || p >= (int)C2D_SpriteSheetCount(sheet)) return;
	C2D_Image im = C2D_SpriteSheetGetImage(sheet, p);
	float sx = w / im.subtex->width, sy = h / im.subtex->height;
	C2D_DrawImageAt(im, x, y, 0.5f, NULL, sx, sy);
}

static float pw(int p) { return sheet && p >= 0 ? C2D_SpriteSheetGetImage(sheet, p).subtex->width : 4; }
static float ph(int p) { return sheet && p >= 0 ? C2D_SpriteSheetGetImage(sheet, p).subtex->height : 4; }

// A frame of 8 pieces (corners as they are, the edges stretched) round x, y, w, h.
static void frame(const char* kind, float x, float y, float w, float h)
{
	char n[48];
	int p[8];
	static const char* const part[8] = { "top", "bottom", "left", "right", "top_left_corner", "top_right_corner",
	                                     "bottom_left_corner", "bottom_right_corner" };
	for (int i = 0; i < 8; i++) { snprintf(n, sizeof n, "%s_%s", kind, part[i]); p[i] = piece(n); }
	float c = pw(p[4]), t = ph(p[0]), l = pw(p[2]);
	img(p[0], x + c, y, w - 2 * c, t);
	img(p[1], x + c, y + h - t, w - 2 * c, t);
	img(p[2], x, y + c, l, h - 2 * c);
	img(p[3], x + w - l, y + c, l, h - 2 * c);
	img(p[4], x, y, c, c);
	img(p[5], x + w - c, y, c, c);
	img(p[6], x, y + h - c, c, c);
	img(p[7], x + w - c, y + h - c, c, c);
}

static void text(const char* s, float x, float y, float size, u32 col, bool centre)
{
	C2D_Text t;
	C2D_TextParse(&t, tbuf, s);
	C2D_TextOptimize(&t);
	float w = 0;
	if (centre) C2D_TextGetDimensions(&t, size, size, &w, NULL);
	C2D_DrawText(&t, C2D_WithColor, centre ? x - w / 2 : x, y, 0.5f, size, size, col);
}

// layout
#define GX 12
#define GY 30
#define CELLW 42
#define COLS 7
#define BY 200
#define BW 76
#define BH 34

void uiDraw(const UiState* s)
{
	if (!ready) return;
	C2D_TextBufClear(tbuf);
	C2D_Prepare();
	C2D_TargetClear(bot, C2D_Color32(0, 0, 0, 0xFF));
	C2D_SceneBegin(bot);
	// the window
	C2D_DrawRectSolid(4, 4, 0.4f, 312, 190, C2D_Color32(0x0C, 0x0A, 0x08, 0xFF));
	frame("thick", 2, 2, 316, 194);
	text("Ekwipunek", 160, 7, 0.55f, GOLD, true);
	if (s->npages > 1) {   // the pages: < 1/3 >
		char b[16];
		snprintf(b, sizeof b, "%d/%d", s->page + 1, s->npages);
		text(b, 262, 7, 0.45f, DIM, true);
		text("<", 22, 4, 0.7f, s->page > 0 ? GOLD : DIM, true);
		text(">", 298, 4, 0.7f, s->page + 1 < s->npages ? GOLD : DIM, true);
	}
	// the items
	int eq = piece("menu_icon_equip");
	for (int i = 0; i < s->nitems; i++) {
		float x = GX + (i % COLS) * CELLW, y = GY + (i / COLS) * CELLW;
		C2D_DrawRectSolid(x, y, 0.45f, 38, 38, C2D_Color32(0x1E, 0x19, 0x12, 0xFF));
		if (s->item[i].on) img(eq, x + 1, y + 1, 36, 36);
		if (i == s->selected) {
			C2D_DrawRectSolid(x - 1, y - 1, 0.44f, 40, 1, GOLD); C2D_DrawRectSolid(x - 1, y + 38, 0.44f, 40, 1, GOLD);
			C2D_DrawRectSolid(x - 1, y - 1, 0.44f, 1, 40, GOLD); C2D_DrawRectSolid(x + 38, y - 1, 0.44f, 1, 40, GOLD);
		}
		img(piece(s->item[i].icon), x + 3, y + 3, 32, 32);
	}
	// what was touched, the numbers
	if (s->selected >= 0 && s->selected < s->nitems) {
		char b[64];
		snprintf(b, sizeof b, "%s%s", s->item[s->selected].label, s->item[s->selected].on ? "  (zaĹ‚oĹĽone)" : "");
		text(b, 14, 158, 0.5f, PALE, false);
	}
	text(s->status, 14, 176, 0.42f, DIM, false);
	// the buttons
	static const char* const blabel[4] = { NULL, "Pochodnia", "+5 NPC", "Bez NPC" };
	for (int k = 0; k < 4; k++) {
		float x = 4 + k * (BW + 3), y = BY;
		bool lit = (k == 1 && s->torch);
		C2D_DrawRectSolid(x + 2, y + 2, 0.4f, BW - 4, BH - 4, lit ? C2D_Color32(0x3A, 0x2A, 0x12, 0xFF) : C2D_Color32(0x14, 0x11, 0x0C, 0xFF));
		frame("button", x, y, BW, BH);
		char b[32];
		if (k == 0) snprintf(b, sizeof b, "Kamera %s", s->camName);
		text(k == 0 ? b : blabel[k], x + BW / 2, y + 10, 0.45f, lit ? PALE : GOLD, true);
	}
}

int uiTouch(const UiState* s, int px, int py)
{
	if (py < 26 && s->npages > 1) {
		if (px < 50) return UI_PREV;
		if (px > 270) return UI_NEXT;
	}
	if (py >= BY && py < BY + BH) {
		int k = (px - 4) / (BW + 3);
		if (k >= 0 && k < 4) return UI_CAMERA + k;
		return UI_NONE;
	}
	if (px >= GX && py >= GY) {
		int c = (px - GX) / CELLW, r = (py - GY) / CELLW;
		if (c < COLS && (px - GX) % CELLW < 38 && (py - GY) % CELLW < 38) {
			int i = r * COLS + c;
			if (i < s->nitems) return UI_ITEM + i;
		}
	}
	return UI_NONE;
}
