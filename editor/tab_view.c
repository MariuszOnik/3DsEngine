// The "Widok" tab: how the editor shows the world (the water's ripples and depth colour). Remembered in
// sdmc:/3ds/mwedit/settings.txt, written when a value is let go.
#include <stdio.h>
#include <string.h>

#include "editor.h"
#include "render.h"
#include "ui.h"
#include "water.h"

#define SETTINGS "sdmc:/3ds/mwedit/settings.txt"

static void apply(Editor* e)
{
	waterSetRipples(e->ripples);
	renderSetWaterDepth(e->waterDepth);
}

void settingsLoad(Editor* e)
{
	e->ripples = 200 / 255.0f;
	e->waterDepth = 800;
	FILE* f = fopen(SETTINGS, "r");
	if (f) {
		char key[32];
		float v;
		while (fscanf(f, "%31s %f", key, &v) == 2) {
			if (!strcmp(key, "fale")) e->ripples = v;
			else if (!strcmp(key, "glebia_wody")) e->waterDepth = v;
		}
		fclose(f);
	}
	apply(e);
}

static void settingsSave(const Editor* e)
{
	FILE* f = fopen(SETTINGS, "w");
	if (!f) return;
	fprintf(f, "fale %.3f\nglebia_wody %.0f\n", e->ripples, e->waterDepth);
	fclose(f);
}

void tabView(Editor* e)
{
	static bool changing;
	char txt[16];
	uiText(10, 34, 0.48f, UI_TEXT, "Woda");
	snprintf(txt, sizeof txt, "%.0f%%", e->ripples * 100);
	bool a = uiSlider((UiRect){ 8, 56, 304, 26 }, "Fale", txt, &e->ripples, 0, 1);
	snprintf(txt, sizeof txt, "%.0f", e->waterDepth);
	bool b = uiSlider((UiRect){ 8, 88, 304, 26 }, "Turkus od", txt, &e->waterDepth, 100, 3000);
	if (a || b) { apply(e); changing = true; }
	else if (changing) { settingsSave(e); changing = false; }   // let go: remember it
	uiText(10, 124, 0.40f, UI_TEXT_DIM, "Fale: jak mocno zmarszczki rozjaśniają i cieniują dno.");
	uiText(10, 140, 0.40f, UI_TEXT_DIM, "Turkus od: na jakiej głębokości woda jest w pełni turkusowa.");
	uiText(10, 164, 0.40f, UI_TEXT_DIM, "3D: suwak na konsoli.");
}
