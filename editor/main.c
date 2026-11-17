// mwedit: the world editor, a program of its own (docs/POMYSLY.md 8). Stage 1: terrain.
//
// Modules: cell (the edited cell in the game's formats), brush (the tools), view (the camera), render (drawing).
// This file: the loop, the editor's own controls and the bottom screen.
//
// Controls:
//   circle pad  move the view            L / R      turn it
//   touch screen: drag up / down tilts the view, sideways turns it
//   D-pad up/dn zoom                     D-pad l/r  brush smaller / bigger
//   A raise   B lower   Y smooth   X flatten (to the height under the cross when X went down)
//   SELECT      brush strength (gentle / strong)       START quit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>

#include "brush.h"
#include "cell.h"
#include "render.h"
#include "view.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static ECell cell;

static float msSince(u64 t0) { return (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC; }

static void waitStart(void)
{
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gspWaitForVBlank();
	}
}

static void bottomScreen(const View* v, const Brush* b, Tool tool, bool strong, float fps, float brushMs, float prepMs)
{
	printf("\x1b[1;1H 3dsOpenworld - edytor terenu\n\n");
	printf(" Komorka %d,%d (pusta)                \n", cell.gx, cell.gy);
	printf(" Pod krzyzykiem: %6.0f              \n", cellHeightAt(&cell, v->tx, v->ty));
	printf(" Pedzel %5.0f   sila %s          \n", b->radius, strong ? "mocna  " : "lagodna");
	printf(" Narzedzie: %-10s                \n", toolName(tool));
	printf(" %4.1f fps   pedzel %4.1f ms        \n", fps, brushMs);
	printf(" Przygotowanie komorki %.1f s     \n\n", prepMs / 1000.0f);
	printf(" Kamera: kat %3.0f stopni         \n\n", -v->pitch * 57.2958f);
	printf(" Galka    przesun     L/R  obrot\n");
	printf(" Dotyk    przeciagnij: kat, obrot\n");
	printf(" D-pad    gora/dol: zoom\n");
	printf("          lewo/prawo: pedzel\n");
	printf(" A podnies    B obniz\n");
	printf(" Y wygladz    X wyrownaj\n");
	printf(" SELECT sila  START wyjscie\n");
}

int main(void)
{
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	printf("\n 3dsOpenworld - edytor terenu\n\n Przygotowuje pusta komorke...\n");
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	u64 t0 = svcGetSystemTick();
	bool ok = renderInit() && cellNew(&cell, 0, 0);
	float prepMs = msSince(t0);
	if (!ok) {
		printf("\n Brak pamieci (%lu KB wolne).\n START: wyjscie\n", (unsigned long)(linearSpaceFree() / 1024));
		waitStart();
		C3D_Fini();
		gfxExit();
		return 0;
	}
	consoleClear();

	View view;
	viewInit(&view);
	Brush brush = { 768.0f, 400.0f, 0.0f };
	bool strong = false;
	u64 last = svcGetSystemTick();
	int frames = 0;
	float fps = 0, fpsAcc = 0, brushMs = 0;

	while (aptMainLoop()) {
		u64 now = svcGetSystemTick();
		float dt = (now - last) / (float)SYSCLOCK_ARM11;
		last = now;
		if (dt > 0.1f) dt = 0.1f;

		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;
		if (down & KEY_SELECT) { strong = !strong; brush.rate = strong ? 1600.0f : 400.0f; }
		if (down & KEY_DRIGHT) { brush.radius *= 1.25f; if (brush.radius > 4096) brush.radius = 4096; }
		if (down & KEY_DLEFT) { brush.radius /= 1.25f; if (brush.radius < 128) brush.radius = 128; }

		circlePosition cp;
		hidCircleRead(&cp);
		float px = cp.dx / 156.0f, py = cp.dy / 156.0f;
		if (fabsf(px) < 0.1f) px = 0;
		if (fabsf(py) < 0.1f) py = 0;
		// the touch screen: dragging turns and tilts the view
		static int lastTx = -1, lastTy = -1;
		if (held & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			if (lastTx >= 0 && !(down & KEY_TOUCH)) viewDrag(&view, tp.px - lastTx, tp.py - lastTy);
			lastTx = tp.px;
			lastTy = tp.py;
		} else
			lastTx = lastTy = -1;
		float turn = (held & KEY_R ? 1.0f : 0) - (held & KEY_L ? 1.0f : 0);
		float zoom = (held & KEY_DDOWN ? 1.0f : 0) - (held & KEY_DUP ? 1.0f : 0);
		viewUpdate(&view, &cell, px, py, turn, zoom, dt);

		Tool tool = (held & KEY_A) ? TOOL_RAISE : (held & KEY_B) ? TOOL_LOWER : (held & KEY_Y) ? TOOL_SMOOTH
		          : (held & KEY_X) ? TOOL_FLATTEN : TOOL_NONE;
		if (down & KEY_X) brush.flatZ = cellHeightAt(&cell, view.tx, view.ty);
		if (tool != TOOL_NONE) {
			u64 b0 = svcGetSystemTick();
			brushApply(&cell, tool, &brush, view.tx, view.ty, dt);
			brushMs = msSince(b0);
		}
		renderSetBrush(&cell, view.tx, view.ty, brush.radius);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x8CA8C4FF, 0);
		C3D_FrameDrawOn(top);
		renderScene(&cell, &view, true);
		C3D_FrameEnd(0);

		frames++;
		fpsAcc += dt;
		if (fpsAcc >= 0.5f) {
			fps = frames / fpsAcc;
			frames = 0;
			fpsAcc = 0;
			bottomScreen(&view, &brush, tool, strong, fps, brushMs, prepMs);
		}
	}
	C3D_Fini();
	gfxExit();
	return 0;
}
