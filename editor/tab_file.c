// The "Plik" tab: saving the cell into the world's .esm, loading it back, starting the cell anew.
//
// The .esm holds what the Construction Set and OpenMW-CS know: heights and the 16 x 16 texture table (VTEX).
// The painted image is finer than that table, so it goes beside it: world/land/<x>_<y>.etc1 (the GPU texture with
// all levels, as the console draws it). Without that file the image is made from the table again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "editor.h"
#include "esp.h"
#include "landtex.h"
#include "ui.h"

#define LAND_DIR WORLD_DIR "/land"
#define IMAGE_MAGIC "MWI1"   // + u32 width, u32 height, u32 bytes, then the ETC1 levels

static float heights[CELL_GRID * CELL_GRID];   // big for the stack; one save / load at a time

static const char* texFile(int i)
{
	const LandTex* t = ltGet(i);
	return t ? t->file : NULL;
}

static const u8* texPixels(int i)
{
	const LandTex* t = ltGet(i);
	return t ? t->rgb : NULL;
}

static void imagePath(const Editor* e, char* out, size_t size)
{
	snprintf(out, size, LAND_DIR "/%d_%d.etc1", e->cell.gx, e->cell.gy);
}

// The first saves went to world.esp: it becomes world.esm (the next save sets its master flag).
static void renameOld(void)
{
	FILE* f = fopen(WORLD_ESP, "rb");
	if (f) { fclose(f); return; }
	rename(WORLD_OLD, WORLD_ESP);
}

static bool saveImage(Editor* e)
{
	char path[96];
	imagePath(e, path, sizeof path);
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	u32 head[3] = { CELL_TEX, CELL_TEX, (u32)cellImageSize() };
	bool ok = fwrite(IMAGE_MAGIC, 1, 4, f) == 4 && fwrite(head, 4, 3, f) == 3 &&
	          fwrite(e->cell.tex.data, 1, head[2], f) == head[2];
	return fclose(f) == 0 && ok;
}

static bool loadImage(Editor* e)
{
	char path[96];
	imagePath(e, path, sizeof path);
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	char magic[4];
	u32 head[3];
	bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, IMAGE_MAGIC, 4) == 0 && fread(head, 4, 3, f) == 3 &&
	          head[0] == CELL_TEX && head[1] == CELL_TEX && head[2] == cellImageSize();
	u8* data = ok ? malloc(head[2]) : NULL;
	ok = data && fread(data, 1, head[2], f) == head[2];
	fclose(f);
	if (ok) ok = cellSetImage(&e->cell, data, head[2]);
	free(data);
	return ok;
}

bool fileSave(Editor* e)
{
	mkdir("sdmc:/3ds/mwedit", 0777);
	mkdir(WORLD_DIR, 0777);
	mkdir(LAND_DIR, 0777);
	renameOld();
	cellFlushPaint(&e->cell, 0);   // all of the painting into the GPU texture first
	bool ok = espSaveLand(WORLD_ESP, e->cell.gx, e->cell.gy, e->cell.h, e->cell.vtex, texFile, e->fileMsg,
	                      sizeof e->fileMsg);
	if (ok && !saveImage(e)) {
		snprintf(e->fileMsg, sizeof e->fileMsg, "Teren zapisany, obrazek nie");
		ok = false;
	}
	// back from the file: the .esm keeps heights in steps of 8 and cuts slopes steeper than 127 steps per 128 units,
	// so the land on the screen is the one in the file
	if (espLoadLand(WORLD_ESP, e->cell.gx, e->cell.gy, heights, NULL, NULL) == 1) cellSetHeights(&e->cell, heights);
	return ok;
}

int fileLoad(Editor* e)
{
	renameOld();
	int r = espLoadLand(WORLD_ESP, e->cell.gx, e->cell.gy, heights, e->cell.vtex, ltFind);
	if (r == 1) {
		cellSetHeights(&e->cell, heights);
		bool img = loadImage(e);
		if (!img) cellImageFromVtex(&e->cell, texPixels);
		snprintf(e->fileMsg, sizeof e->fileMsg, img ? "Wczytano komórkę %d,%d" : "Wczytano komórkę %d,%d (tekstury z kafli)",
		         e->cell.gx, e->cell.gy);
	} else if (r == 0)
		snprintf(e->fileMsg, sizeof e->fileMsg, "W pliku nie ma komórki %d,%d", e->cell.gx, e->cell.gy);
	else
		snprintf(e->fileMsg, sizeof e->fileMsg, "Nie ma jeszcze pliku świata");
	return r;
}

void tabFile(Editor* e)
{
	if (uiButton((UiRect){ 8, 36, 148, 40 }, ICON_NONE, "Zapisz", false)) fileSave(e);
	if (uiButton((UiRect){ 164, 36, 148, 40 }, ICON_NONE, "Wczytaj", false)) fileLoad(e);
	if (uiButton((UiRect){ 8, 84, 148, 40 }, ICON_NONE, "Wyczyść komórkę", false)) {
		for (int i = 0; i < CELL_GRID * CELL_GRID; i++) heights[i] = NEW_LAND_Z;
		cellSetHeights(&e->cell, heights);
		snprintf(e->fileMsg, sizeof e->fileMsg, "Płaski teren (zapisz, żeby został)");
	}
	uiText(10, 136, 0.5f, UI_TEXT, "%s", e->fileMsg[0] ? e->fileMsg : " ");
	uiText(10, 166, 0.42f, UI_TEXT_DIM, "Plik gry: 3ds/mwedit/world/world.esm");
	uiText(10, 184, 0.42f, UI_TEXT_DIM, "W OpenMW-CS otwórz go jako plik gry (Game file).");
	if (e->texFailed)
		uiText(10, 200, 0.42f, UI_TEXT_DIM, "Tekstury: %d wczytane, %d plików nie do odczytu", e->ntex, e->texFailed);
}
