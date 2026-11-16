// Dynamic objects (docs/PLAN.md step 1): items, doors and containers with their persistent id (plugin, FRMR),
// from romfs:/objects.bin (tools/mwbake.py write_objects). Their state is always in RAM, also for cells that are
// not loaded; a cell only decides whether they are drawn. The models are shared by every copy.
#pragma once
#include <3ds.h>
#include <stdbool.h>

enum { OT_ITEM = 1, OT_DOOR = 2, OT_CONT = 3 };
enum { OF_TAKEN = 1, OF_OPEN = 2, OF_LOCKED = 4, OF_CHANGED = 0x8000 };   // CHANGED: differs from the baked data (to save)

typedef struct { u32 idOfs, nameOfs; u16 type, model; } __attribute__((packed)) ObjBase;
typedef struct { u16 first, nparts; float quant, radius; } __attribute__((packed)) ObjModel;
typedef struct { u16 tex; u8 flags, pad; u32 vofs, vcount, iofs, icount; } __attribute__((packed)) ObjPart;
typedef struct { u32 frmr; u16 plugin, base; s16 gx, gy; float pos[3], rot[3], scale; u32 destOfs; } __attribute__((packed)) ObjRec;

typedef struct {
	u32 frmr; u16 plugin, base;
	s16 gx, gy;                // the cell it belongs to
	float pos[3], rot[3], scale;
	u16 flags;
	float open;                // doors: 0 shut .. 1 open (animated towards the OF_OPEN flag)
	u32 destOfs;               // a load door's destination (objDest), 0xFFFFFFFF: not a load door
} WObj;

extern WObj* objs;
extern int nobj;
extern const ObjBase* objBases;
extern const ObjModel* objModels;
extern const ObjPart* objParts;
extern u8* objData;            // the whole file in linear memory (the parts' vertices and indices point into it)
extern int objNModels, objNParts;
extern u32 objBytes;

bool objLoad(void);
const char* objId(int i);      // the base record's id ("barrel_01")
const char* objName(int i);    // the name shown ("Barrel")
int objType(int i);
// A load door (Morrowind's doors outside lead into interiors: they do not swing): where to, "" = outside.
// NULL: not a load door.
const char* objDest(int i);
const ObjModel* objModel(int i);
int objFind(const char* id, int after);   // the next object (> after) of that base id, -1 none
// The object the player can use: the nearest one within reach of the feet (x, y, z), more or less in front
// (facing yaw: 0 = +y), not taken. -1 none.
int objNearest(const float feet[3], float yaw, float reach);
void objUpdate(float dt);      // doors swing
// The object's matrix (3 rows x 4: the turn x scale x the model's quantisation, the position - eye).
void objMatrix(int i, const float eye[3], float out[12]);
