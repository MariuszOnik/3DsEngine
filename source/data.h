// Where the baked data is read from. sdmc:/3ds/mwcell3d/data/ when it holds index.bin (uploaded once, again only
// when the bake changes: the app is then built without romfs, ~1-2 MB to upload), else romfs:/ (the data packed
// into the .3dsx, as before).
#pragma once

#define SD_DATA "sdmc:/3ds/mwcell3d/data/"
extern char dataDir[40];                 // "sdmc:/3ds/mwcell3d/data/" or "romfs:/"
void dataInit(void);                     // after romfsInit, before anything is loaded
const char* dataPath(const char* name);  // dataDir + name (a ring of buffers: a few may be in use at once)
