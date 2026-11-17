// The editor's touch-screen widgets (citro2d), in its own plain style (no game data needed).
// Immediate mode: each frame the tab calls the widgets, which handle the touch and, when the screen is being
// redrawn this frame (uiDrawing()), draw themselves. The bottom screen is redrawn only when something changed.
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define UI_W 320
#define UI_H 240

typedef struct { int x, y, w, h; } UiRect;

// The theme
#define UI_BG        C2D_Color32(0x1E, 0x21, 0x27, 0xFF)
#define UI_PANEL     C2D_Color32(0x2B, 0x30, 0x38, 0xFF)
#define UI_PANEL_HI  C2D_Color32(0x38, 0x3F, 0x4A, 0xFF)
#define UI_ACCENT    C2D_Color32(0x4F, 0xA3, 0xE0, 0xFF)
#define UI_TEXT      C2D_Color32(0xE6, 0xE8, 0xEB, 0xFF)
#define UI_TEXT_DIM  C2D_Color32(0x8A, 0x92, 0x9C, 0xFF)

typedef enum { ICON_NONE, ICON_UP, ICON_DOWN, ICON_SMOOTH, ICON_FLAT } UiIcon;

void uiInit(void);
void uiExit(void);

// Frame (between C3D_FrameBegin and C3D_FrameEnd): uiBegin with the buttons, the widgets, uiEnd. The screen is
// redrawn when `redraw` or something asked for it (uiInvalidate, a tap); otherwise it keeps its last picture.
void uiBegin(u32 down, u32 held, bool redraw);
void uiEnd(void);
bool uiDrawing(void);
void uiInvalidate(void);   // redraw next frame
bool uiWantsRedraw(void);
bool uiTouching(void);     // the stylus is on the screen this frame

void uiText(float x, float y, float size, u32 colour, const char* fmt, ...);
void uiTextCentred(UiRect r, float size, u32 colour, const char* text);
void uiPanel(UiRect r, u32 colour);

// A row of tabs; returns the selected one (changed by a tap).
int uiTabs(UiRect r, const char* const* labels, int n, int selected);
// A button with an icon above its label; true when tapped.
bool uiButton(UiRect r, UiIcon icon, const char* label, bool selected);
// A slider: dragging along it sets *value in [min, max]; true while it changes it.
bool uiSlider(UiRect r, const char* label, const char* valueText, float* value, float min, float max);
// An area to drag on: the movement this frame in pixels; true while dragged.
bool uiDragArea(UiRect r, const char* hint, float* dx, float* dy);
