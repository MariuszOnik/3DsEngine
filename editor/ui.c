#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static C3D_RenderTarget* target;
static C2D_TextBuf textBuf;
static bool drawing, wantRedraw = true;
static u32 kDown, kHeld;
static touchPosition touch, lastTouch;
static bool hadTouch;
static int active = -1;    // the widget the stylus went down on (by its position: x * 1000 + y)

void uiInit(void)
{
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	textBuf = C2D_TextBufNew(4096);
}

void uiExit(void)
{
	C2D_TextBufDelete(textBuf);
	C2D_Fini();
}

void uiInvalidate(void) { wantRedraw = true; }
bool uiWantsRedraw(void) { return wantRedraw; }
bool uiDrawing(void) { return drawing; }
bool uiTouching(void) { return (kHeld & KEY_TOUCH) != 0; }

void uiBegin(u32 down, u32 held, bool redraw)
{
	kDown = down;
	kHeld = held;
	lastTouch = touch;
	if (held & KEY_TOUCH) hidTouchRead(&touch);
	if (down & KEY_TOUCH) lastTouch = touch;
	if (!(held & KEY_TOUCH)) {
		if (hadTouch) wantRedraw = true;   // let go: the pressed look goes
		active = -1;
	}
	hadTouch = (held & KEY_TOUCH) != 0;
	drawing = redraw || wantRedraw;
	if (drawing) {
		wantRedraw = false;
		C2D_TextBufClear(textBuf);
		C2D_Prepare();
		C2D_TargetClear(target, UI_BG);
		C2D_SceneBegin(target);
	}
}

void uiEnd(void)
{
	if (drawing) C2D_Flush();
}

static bool inside(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static int keyOf(UiRect r) { return r.x * 1000 + r.y; }

// The stylus went down on r this frame (it becomes the active widget).
static bool pressedOn(UiRect r)
{
	if ((kDown & KEY_TOUCH) && inside(r, touch.px, touch.py)) {
		active = keyOf(r);
		wantRedraw = true;
		return true;
	}
	return false;
}

static bool activeOn(UiRect r) { return (kHeld & KEY_TOUCH) && active == keyOf(r); }

void uiPanel(UiRect r, u32 colour)
{
	if (drawing) C2D_DrawRectSolid(r.x, r.y, 0, r.w, r.h, colour);
}

void uiText(float x, float y, float size, u32 colour, const char* fmt, ...)
{
	if (!drawing) return;
	char s[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof s, fmt, ap);
	va_end(ap);
	C2D_Text t;
	C2D_TextParse(&t, textBuf, s);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor, x, y, 0, size, size, colour);
}

void uiTextCentred(UiRect r, float size, u32 colour, const char* text)
{
	if (!drawing) return;
	C2D_Text t;
	C2D_TextParse(&t, textBuf, text);
	C2D_TextOptimize(&t);
	float w, h;
	C2D_TextGetDimensions(&t, size, size, &w, &h);
	C2D_DrawText(&t, C2D_WithColor, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, 0, size, size, colour);
}

int uiTabs(UiRect r, const char* const* labels, int n, int selected)
{
	int w = r.w / n;
	for (int i = 0; i < n; i++) {
		UiRect t = { r.x + i * w, r.y, i == n - 1 ? r.w - i * w : w, r.h };
		if (labels[i][0] && pressedOn(t)) selected = i;
		if (drawing) {
			uiPanel(t, i == selected ? UI_PANEL_HI : UI_PANEL);
			if (i == selected) C2D_DrawRectSolid(t.x, t.y + t.h - 3, 0, t.w, 3, UI_ACCENT);
			if (i > 0) C2D_DrawRectSolid(t.x, t.y + 4, 0, 1, t.h - 8, UI_BG);
			uiTextCentred(t, 0.5f, labels[i][0] ? (i == selected ? UI_TEXT : UI_TEXT_DIM) : UI_TEXT_DIM, labels[i]);
		}
	}
	return selected;
}

static void drawIcon(UiIcon icon, float cx, float cy, u32 c)
{
	switch (icon) {
	case ICON_UP: C2D_DrawTriangle(cx, cy - 8, c, cx - 9, cy + 6, c, cx + 9, cy + 6, c, 0); break;
	case ICON_DOWN: C2D_DrawTriangle(cx - 9, cy - 6, c, cx + 9, cy - 6, c, cx, cy + 8, c, 0); break;
	case ICON_SMOOTH:   // a wave of small steps
		for (int i = 0; i < 6; i++) {
			float h = 4 + 3 * (i == 2 || i == 3 ? 1 : 0) + (i == 1 || i == 4 ? 1.5f : 0);
			C2D_DrawRectSolid(cx - 12 + i * 4, cy + 6 - h, 0, 3, h, c);
		}
		break;
	case ICON_FLAT:
		C2D_DrawRectSolid(cx - 11, cy - 4, 0, 22, 3, c);
		C2D_DrawRectSolid(cx - 11, cy + 2, 0, 22, 3, c);
		break;
	default: break;
	}
}

bool uiButton(UiRect r, UiIcon icon, const char* label, bool selected)
{
	bool tapped = pressedOn(r);
	if (drawing) {
		bool down = activeOn(r);
		uiPanel(r, down ? UI_ACCENT : selected ? UI_PANEL_HI : UI_PANEL);
		if (selected && !down) {   // a frame in the accent colour
			C2D_DrawRectSolid(r.x, r.y, 0, r.w, 2, UI_ACCENT);
			C2D_DrawRectSolid(r.x, r.y + r.h - 2, 0, r.w, 2, UI_ACCENT);
			C2D_DrawRectSolid(r.x, r.y, 0, 2, r.h, UI_ACCENT);
			C2D_DrawRectSolid(r.x + r.w - 2, r.y, 0, 2, r.h, UI_ACCENT);
		}
		u32 c = selected || down ? UI_TEXT : UI_TEXT_DIM;
		if (icon != ICON_NONE) {
			drawIcon(icon, r.x + r.w / 2.0f, r.y + r.h * 0.38f, c);
			UiRect lr = { r.x, r.y + r.h / 2, r.w, r.h / 2 };
			uiTextCentred(lr, 0.42f, c, label);
		} else
			uiTextCentred(r, 0.5f, c, label);
	}
	return tapped;
}

bool uiSlider(UiRect r, const char* label, const char* valueText, float* value, float min, float max)
{
	UiRect bar = { r.x + 70, r.y, r.w - 70 - 56, r.h };
	pressedOn(bar);
	bool changing = activeOn(bar);
	if (changing) {
		float t = (touch.px - bar.x) / (float)bar.w;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		float v = min + (max - min) * t;
		if (v != *value) { *value = v; wantRedraw = true; }
	}
	if (drawing) {
		uiText(r.x, r.y + r.h / 2 - 8, 0.48f, UI_TEXT_DIM, "%s", label);
		float t = (*value - min) / (max - min);
		float cy = bar.y + bar.h / 2.0f;
		C2D_DrawRectSolid(bar.x, cy - 2, 0, bar.w, 4, UI_PANEL_HI);
		C2D_DrawRectSolid(bar.x, cy - 2, 0, bar.w * t, 4, UI_ACCENT);
		C2D_DrawRectSolid(bar.x + bar.w * t - 5, cy - 9, 0, 10, 18, changing ? UI_TEXT : UI_ACCENT);
		uiText(bar.x + bar.w + 8, r.y + r.h / 2 - 8, 0.48f, UI_TEXT, "%s", valueText);
	}
	return changing;
}

bool uiDragArea(UiRect r, const char* hint, float* dx, float* dy)
{
	pressedOn(r);
	bool dragging = activeOn(r) && !(kDown & KEY_TOUCH);
	*dx = dragging ? (float)(touch.px - lastTouch.px) : 0;
	*dy = dragging ? (float)(touch.py - lastTouch.py) : 0;
	if (drawing) {
		uiPanel(r, activeOn(r) ? UI_PANEL_HI : UI_PANEL);
		uiTextCentred(r, 0.45f, UI_TEXT_DIM, hint);
	}
	return dragging;
}
