// mwedit: the world editor, a program of its own (docs/POMYSLY.md 8, docs/SILNIK.md). Stage 1: terrain.
//
// Modules: cell (the edited cell in the game's formats), brush (the tools), view (the camera), render (the 3D on the
// top screen), ui (touch-screen widgets), esp (the world saved as an .esp plugin), tab_* (the bottom screen's tabs). This file: the loop and the buttons.
//
// Buttons (the touch screen has the rest: tools, size, strength, camera angle):
//   circle pad  move the view            L / R      turn it
//   D-pad up/dn zoom                     D-pad l/r  brush smaller / bigger
//   A the tool chosen on the touch screen (raise at first; on "Malowanie": paint)   B lower   Y smooth   X flatten
//   SELECT      brush strength gentle / strong                START quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>

#include "editor.h"
#include "landtex.h"
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
	case TAB_PAINT: tabPaint(&ed); break;
	case TAB_OBJECTS: tabLater(&ed, "Obiekty: domy, drzewa, skały"); break;
	case TAB_FILE: tabFile(&ed); break;
	default: break;
	}
	uiPanel((UiRect){ 0, 216, UI_W, 24 }, UI_PANEL);
	uiText(6, 220, 0.45f, UI_TEXT, "Komórka %d,%d   wys. %.0f   kąt %.0f   %.0f fps", ed.cell.gx, ed.cell.gy,
	       cellHeightAt(&ed.cell, ed.view.tx, ed.view.ty), -ed.view.pitch * 57.2958f, ed.fps);
	uiEnd();
}

// A check without touching anything (Azahar): with sdmc:/3ds/mwedit/painttest.txt the editor paints a few strokes
// with the first textures at the start and opens the "Malowanie" tab; the time of the strokes and of encoding them
// shows on the "Plik" tab's message line.
static void paintTest(void)
{
	FILE* f = fopen("sdmc:/3ds/mwedit/painttest.txt", "rb");
	if (!f) return;
	fclose(f);
	u64 t0 = svcGetSystemTick();
	for (int s = 0; s < 3; s++) {
		const LandTex* lt = ltGet(s % (ltCount() ? ltCount() : 1));
		if (!lt) break;
		for (int k = 0; k < 40; k++) {
			float a = k / 40.0f * 3.0f + s * 2.1f;
			cellPaint(&ed.cell, lt->rgb, s, cosf(a) * (900 + s * 700), sinf(a * 0.7f) * (900 + s * 500), 700, 0.35f);
		}
	}
	float paintMs = msSince(t0);
	t0 = svcGetSystemTick();
	int blocks = ed.cell.ndirty;
	cellFlushPaint(&ed.cell, 0);
	snprintf(ed.fileMsg, sizeof ed.fileMsg, "test: 120 pociągnięć %.0f ms, %d bloków ETC1 %.0f ms", paintMs, blocks,
	         msSince(t0));
	f = fopen("sdmc:/3ds/mwedit/painttest.log", "w");
	if (f) { fprintf(f, "%s\n", ed.fileMsg); fclose(f); }
	ed.tab = TAB_PAINT;
	ed.view.tx = 0;
	ed.view.ty = 0;
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
	ed.ntex = ltLoadAll(&ed.texFailed);   // the land textures (before loading: the cell names them)
	fileLoad(&ed);   // the cell as last saved, if the world file has it
	uiInit();   // the touch screen is citro2d's from here on

	viewInit(&ed.view);
	ed.brush = (Brush){ 768.0f, 400.0f, 0.0f };
	ed.aTool = TOOL_RAISE;
	ed.tab = TAB_TERRAIN;
	ed.paintRate = 0.5f;
	paintTest();
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

		bool painting = ed.tab == TAB_PAINT;   // A paints there; B, Y, X still sculpt
		ed.tool = (held & KEY_A) && !painting ? ed.aTool : (held & KEY_B) ? TOOL_LOWER : (held & KEY_Y) ? TOOL_SMOOTH
		        : (held & KEY_X) ? TOOL_FLATTEN : TOOL_NONE;
		if (ed.tool == TOOL_FLATTEN && lastTool != TOOL_FLATTEN) ed.brush.flatZ = cellHeightAt(&ed.cell, ed.view.tx, ed.view.ty);
		if (ed.tool != lastTool) changed = true;
		lastTool = ed.tool;
		if (ed.tool != TOOL_NONE) {
			u64 b0 = svcGetSystemTick();
			brushApply(&ed.cell, ed.tool, &ed.brush, ed.view.tx, ed.view.ty, dt);
			ed.brushMs = msSince(b0);
		}
		if (painting && (held & KEY_A)) {
			const LandTex* lt = ltGet(ed.paintTex);
			if (lt) cellPaint(&ed.cell, lt->rgb, ed.paintTex, ed.view.tx, ed.view.ty, ed.brush.radius, ed.paintRate * PAINT_PER_SEC * dt);
		}
		cellFlushPaint(&ed.cell, 12.0f);   // the painted blocks into the GPU texture, a few each frame
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
