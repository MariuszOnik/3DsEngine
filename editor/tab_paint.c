// The "Malowanie" tab: the palette of land textures (3ds/mwedit/textures/*.png), the brush's size and strength.
// A paints with the chosen texture while this tab is open.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "editor.h"
#include "landtex.h"
#include "ui.h"

#define COLS 4
#define ROWS 2
#define PER_PAGE (COLS * ROWS)

void tabPaint(Editor* e)
{
	int n = ltCount();
	if (n == 0) {
		uiText(10, 50, 0.5f, UI_TEXT, "Brak tekstur");
		uiText(10, 74, 0.42f, UI_TEXT_DIM, "Włóż pliki PNG do 3ds/mwedit/textures/");
		return;
	}
	int pages = (n + PER_PAGE - 1) / PER_PAGE;
	if (e->paintPage >= pages) e->paintPage = pages - 1;

	// the palette: 4 x 2 textures, the page buttons on the right
	for (int k = 0; k < PER_PAGE; k++) {
		int i = e->paintPage * PER_PAGE + k;
		if (i >= n) break;
		const LandTex* t = ltGet(i);
		char label[12];
		snprintf(label, sizeof label, "%s", t->name);   // long names cut
		UiRect r = { 6 + (k % COLS) * 62, 30 + (k / COLS) * 62, 58, 58 };
		if (uiImageButton(r, &t->img, label, i == e->paintTex)) e->paintTex = i;
	}
	char pg[16];
	snprintf(pg, sizeof pg, "%d/%d", e->paintPage + 1, pages);
	if (uiButton((UiRect){ 256, 30, 58, 40 }, ICON_UP, "", false) && e->paintPage > 0) e->paintPage--;
	uiTextCentred((UiRect){ 256, 72, 58, 18 }, 0.42f, UI_TEXT_DIM, pg);
	if (uiButton((UiRect){ 256, 92, 58, 40 }, ICON_DOWN, "", false) && e->paintPage < pages - 1) e->paintPage++;

	// size (shared with sculpting) and strength
	char txt[16];
	float t = logf(e->brush.radius / BRUSH_MIN) / logf(BRUSH_MAX / BRUSH_MIN);
	snprintf(txt, sizeof txt, "%.0f", e->brush.radius);
	if (uiSlider((UiRect){ 8, 158, 304, 26 }, "Rozmiar", txt, &t, 0, 1))
		e->brush.radius = BRUSH_MIN * expf(t * logf(BRUSH_MAX / BRUSH_MIN));
	snprintf(txt, sizeof txt, "%.0f%%", e->paintRate * 100);
	uiSlider((UiRect){ 8, 186, 304, 26 }, "Siła", txt, &e->paintRate, 0.05f, 1.0f);
}
