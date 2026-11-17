// The "Plik" tab: saving the cell into the world's .esp, loading it back, starting the cell anew.
#include <stdio.h>
#include <sys/stat.h>

#include "editor.h"
#include "esp.h"
#include "ui.h"

static float heights[CELL_GRID * CELL_GRID];   // big for the stack; one save / load at a time

bool fileSave(Editor* e)
{
	mkdir("sdmc:/3ds/mwedit", 0777);
	mkdir(WORLD_DIR, 0777);
	bool ok = espSaveLand(WORLD_ESP, e->cell.gx, e->cell.gy, e->cell.h, e->fileMsg, sizeof e->fileMsg);
	// back from the file: the .esp keeps heights in steps of 8 and cuts slopes steeper than 127 steps per 128 units,
	// so the land on the screen is the one in the file
	if (ok && espLoadLand(WORLD_ESP, e->cell.gx, e->cell.gy, heights) == 1) cellSetHeights(&e->cell, heights);
	return ok;
}

int fileLoad(Editor* e)
{
	int r = espLoadLand(WORLD_ESP, e->cell.gx, e->cell.gy, heights);
	if (r == 1) {
		cellSetHeights(&e->cell, heights);
		snprintf(e->fileMsg, sizeof e->fileMsg, "Wczytano komórkę %d,%d", e->cell.gx, e->cell.gy);
	} else if (r == 0)
		snprintf(e->fileMsg, sizeof e->fileMsg, "W pliku nie ma komórki %d,%d", e->cell.gx, e->cell.gy);
	else
		snprintf(e->fileMsg, sizeof e->fileMsg, "Nie ma jeszcze pliku świata");
	return r;
}

void tabFile(Editor* e)
{
	if (uiButton((UiRect){ 8, 36, 148, 40 }, ICON_NONE, "Zapisz .esp", false)) fileSave(e);
	if (uiButton((UiRect){ 164, 36, 148, 40 }, ICON_NONE, "Wczytaj", false)) fileLoad(e);
	if (uiButton((UiRect){ 8, 84, 148, 40 }, ICON_NONE, "Wyczyść komórkę", false)) {
		for (int i = 0; i < CELL_GRID * CELL_GRID; i++) heights[i] = NEW_LAND_Z;
		cellSetHeights(&e->cell, heights);
		snprintf(e->fileMsg, sizeof e->fileMsg, "Płaski teren (zapisz, żeby został)");
	}
	uiText(10, 136, 0.5f, UI_TEXT, "%s", e->fileMsg[0] ? e->fileMsg : " ");
	uiText(10, 166, 0.42f, UI_TEXT_DIM, "Plik: 3ds/mwedit/world/world.esp");
	uiText(10, 184, 0.42f, UI_TEXT_DIM, "Otworzysz go w OpenMW-CS i Construction Secie.");
}
