// The "Teren" tab: the sculpting tools, the brush's size and strength, and an area to drag the camera with.
#include <math.h>
#include <stdio.h>

#include "editor.h"
#include "ui.h"

void tabTerrain(Editor* e)
{
	static const struct { Tool tool; UiIcon icon; const char* label; } tools[] = {
		{ TOOL_RAISE, ICON_UP, "podnies (A)" },
		{ TOOL_LOWER, ICON_DOWN, "obniz (B)" },
		{ TOOL_SMOOTH, ICON_SMOOTH, "wygladz (Y)" },
		{ TOOL_FLATTEN, ICON_FLAT, "wyrownaj (X)" },
	};
	// the tools: tapping one gives it to A (B, Y, X keep their own); the one in use lights up
	for (int i = 0; i < 4; i++) {
		UiRect r = { 6 + i * 78, 32, 74, 48 };
		bool sel = e->tool != TOOL_NONE ? e->tool == tools[i].tool : e->aTool == tools[i].tool;
		if (uiButton(r, tools[i].icon, tools[i].label, sel)) e->aTool = tools[i].tool;
	}

	// size: a log scale (the D-pad steps by 1.25 times), strength: linear
	char txt[16];
	float t = logf(e->brush.radius / BRUSH_MIN) / logf(BRUSH_MAX / BRUSH_MIN);
	snprintf(txt, sizeof txt, "%.0f", e->brush.radius);
	if (uiSlider((UiRect){ 8, 88, 304, 26 }, "Rozmiar", txt, &t, 0, 1))
		e->brush.radius = BRUSH_MIN * expf(t * logf(BRUSH_MAX / BRUSH_MIN));
	snprintf(txt, sizeof txt, "%.0f/s", e->brush.rate);
	uiSlider((UiRect){ 8, 118, 304, 26 }, "Sila", txt, &e->brush.rate, RATE_MIN, RATE_MAX);

	// the camera
	float dx, dy;
	if (uiDragArea((UiRect){ 8, 152, 304, 58 }, "kamera: przeciagnij (kat, obrot)", &dx, &dy)) {
		viewDrag(&e->view, dx, dy);
		uiInvalidate();   // the angle in the status bar
	}
}

void tabFile(Editor* e)
{
	(void)e;
	uiButton((UiRect){ 8, 36, 148, 40 }, ICON_NONE, "Zapisz .esp", false);
	uiButton((UiRect){ 164, 36, 148, 40 }, ICON_NONE, "Wczytaj", false);
	uiButton((UiRect){ 8, 84, 148, 40 }, ICON_NONE, "Nowa komorka", false);
	uiText(10, 140, 0.45f, UI_TEXT_DIM, "Zapis i wczytywanie: nastepny krok");
	uiText(10, 158, 0.45f, UI_TEXT_DIM, "(plik .esp zgodny z OpenMW-CS)");
}

void tabLater(Editor* e, const char* what)
{
	(void)e;
	uiText(10, 60, 0.55f, UI_TEXT, "%s", what);
	uiText(10, 84, 0.45f, UI_TEXT_DIM, "wkrotce");
}
