// mwedit: the world editor, a program of its own (docs/POMYSLY.md 8, docs/SILNIK.md). Stage 1: terrain.
//
// Modules: cell (the edited cell in the game's formats), brush (the tools), view (the camera), render (the 3D on the
// top screen), ui (touch-screen widgets), tab_* (the bottom screen's tabs). This file: the loop and the buttons.
//
// Buttons (the touch screen has the rest: tools, size, strength, camera angle):
//   circle pad  move the view            L / R      turn it
//   D-pad up/dn zoom                     D-pad l/r  brush smaller / bigger
//   A the tool chosen on the touch screen (raise at first)   B lower   Y smooth   X flatten
//   SELECT      brush strength gentle / strong                START quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>

#include "editor.h"
#include "render.h"
#include "ui.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static Editor ed;

static float msSince(u64 t0) { return (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC; }

// The bottom screen: tabs, the tab's widgets, the status bar.
static void bottomScreen(u32 down, u32 held, bool redraw)
{
	static const char* const tabs[TAB_COUNT] = { "Teren", "Malowanie", "Obiekty", "", "Plik" };
	uiBegin(down, held, redraw);
	Tab t = (Tab)uiTabs((UiRect){ 0, 0, UI_W, 26 }, tabs, TAB_COUNT, ed.tab);
	if (t != ed.tab) { ed.tab = t; uiInvalidate(); }
	switch (ed.tab) {
	case TAB_TERRAIN: tabTerrain(&ed); break;
	case TAB_PAINT: tabLater(&ed, "Malowanie tekstur terenu"); break;
	case TAB_OBJECTS: tabLater(&ed, "Obiekty: domy, drzewa, skały"); break;
	case TAB_FILE: tabFile(&ed); break;
	default: break;
	}
	uiPanel((UiRect){ 0, 216, UI_W, 24 }, UI_PANEL);
	uiText(6, 220, 0.45f, UI_TEXT, "Komórka %d,%d   wys. %.0f   kąt %.0f   %.0f fps", ed.cell.gx, ed.cell.gy,
	       cellHeightAt(&ed.cell, ed.view.tx, ed.view.ty), -ed.view.pitch * 57.2958f, ed.fps);
	uiEnd();
}

int main(void)
{
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);   // only while the cell is prepared (then the touch screen's widgets)
	printf("\n 3dsOpenworld - edytor\n\n Przygotowuje pusta komorke...\n");
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	u64 t0 = svcGetSystemTick();
	bool ok = renderInit() && cellNew(&ed.cell, 0, 0);
	ed.prepMs = msSince(t0);
	if (!ok) {
		printf("\n Brak pamieci (%lu KB wolne).\n START: wyjscie\n", (unsigned long)(linearSpaceFree() / 1024));
		while (aptMainLoop()) {
			hidScanInput();
			if (hidKeysDown() & KEY_START) break;
			gspWaitForVBlank();
		}
		C3D_Fini();
		gfxExit();
		return 0;
	}
	uiInit();   // the touch screen is citro2d's from here on

	viewInit(&ed.view);
	ed.brush = (Brush){ 768.0f, 400.0f, 0.0f };
	ed.aTool = TOOL_RAISE;
	ed.tab = TAB_TERRAIN;
	u64 last = svcGetSystemTick();
	int frames = 0;
	float fpsAcc = 0;
	Tool lastTool = TOOL_NONE;

	while (aptMainLoop()) {
		u64 now = svcGetSystemTick();
		float dt = (now - last) / (float)SYSCLOCK_ARM11;
		last = now;
		if (dt > 0.1f) dt = 0.1f;

		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;
		bool changed = false;   // something the bottom screen shows
		if (down & KEY_SELECT) { ed.brush.rate = ed.brush.rate < 1000 ? 1600.0f : 400.0f; changed = true; }
		if (down & KEY_DRIGHT) { ed.brush.radius *= 1.25f; if (ed.brush.radius > BRUSH_MAX) ed.brush.radius = BRUSH_MAX; changed = true; }
		if (down & KEY_DLEFT) { ed.brush.radius /= 1.25f; if (ed.brush.radius < BRUSH_MIN) ed.brush.radius = BRUSH_MIN; changed = true; }

		circlePosition cp;
		hidCircleRead(&cp);
		float px = cp.dx / 156.0f, py = cp.dy / 156.0f;
		if (fabsf(px) < 0.1f) px = 0;
		if (fabsf(py) < 0.1f) py = 0;
		float turn = (held & KEY_R ? 1.0f : 0) - (held & KEY_L ? 1.0f : 0);
		float zoom = (held & KEY_DDOWN ? 1.0f : 0) - (held & KEY_DUP ? 1.0f : 0);
		viewUpdate(&ed.view, &ed.cell, px, py, turn, zoom, dt);

		ed.tool = (held & KEY_A) ? ed.aTool : (held & KEY_B) ? TOOL_LOWER : (held & KEY_Y) ? TOOL_SMOOTH
		        : (held & KEY_X) ? TOOL_FLATTEN : TOOL_NONE;
		if (ed.tool == TOOL_FLATTEN && lastTool != TOOL_FLATTEN) ed.brush.flatZ = cellHeightAt(&ed.cell, ed.view.tx, ed.view.ty);
		if (ed.tool != lastTool) changed = true;
		lastTool = ed.tool;
		if (ed.tool != TOOL_NONE) {
			u64 b0 = svcGetSystemTick();
			brushApply(&ed.cell, ed.tool, &ed.brush, ed.view.tx, ed.view.ty, dt);
			ed.brushMs = msSince(b0);
		}
		renderSetBrush(&ed.cell, ed.view.tx, ed.view.ty, ed.brush.radius);

		frames++;
		fpsAcc += dt;
		if (fpsAcc >= 0.5f) {   // the status bar twice a second
			ed.fps = frames / fpsAcc;
			frames = 0;
			fpsAcc = 0;
			changed = true;
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x8CA8C4FF, 0);
		C3D_FrameDrawOn(top);
		renderScene(&ed.cell, &ed.view, true);
		bottomScreen(down, held, changed || uiTouching());
		C3D_FrameEnd(0);
	}
	uiExit();
	C3D_Fini();
	gfxExit();
	return 0;
}
