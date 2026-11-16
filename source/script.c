// Scripts in MiniSharp2: the language's VM embedded, Game.* natives over the objects (objects.c), the events.
//
// Every file in romfs:/scripts/*.cs is one program. A script is a class derived from ObjectScript that says
// itself what it belongs to (Dla(): "barrel_*, crate_*", an exact id, or #drzwi / #przedmiot / #pojemnik).
// Each object gets ITS OWN copy of the script the first time something happens to it, so a script's fields
// are that door's / that barrel's state (like Morrowind's local variables). No Main and no list of scripts.
// Later the baker will take the script's name from the object's record (OpenMW-CS: the "Script" field).
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "ms.h"
#include "lib.h"
#include "objects.h"
#include "script.h"

float scriptMs;
int scriptCalls;
char scriptSource[48];
static Program* prog;
static VM* vm;
static char err[256];
static int fnUpdate = -1;
static u64 callStart;

void gameFeet(float out[3]);   // main.c: the player's feet

enum { EV_ACTIVATE, EV_TAKE, EV_OPEN, EV_CLOSE, NEV };
static const char* const EVNAME[NEV] = { "OnActivate", "OnTake", "OnOpen", "OnClose" };

#define MAXSCR 64
#define MAXPAT 8
typedef struct {
	int cls;
	char pat[MAXPAT][40];   // an id, "prefix*", "#drzwi" / "#przedmiot" / "#pojemnik"
	int npat;
	int fn[NEV];            // -1: the script does not override it (ObjectScript's empty one is not called)
	bool broken;            // a run-time error: not called again
} Script;
static Script scr[MAXSCR];
static int nscr;

#define MAXINST 1024
typedef struct { int obj, script, handle; } Inst;   // a copy of a script for one object
static Inst inst[MAXINST];
static int ninst;
static int fieldObj = -1;   // ObjectScript.Obj

// the files glued into one source: where each starts (for the errors' line numbers)
#define MAXFILES 32
static char fileName[MAXFILES][48];
static int fileLine[MAXFILES], nfiles;

const char* scriptError(void) { return err; }

// ------------------------------------------------------------------ helpers

static Value vNull(void) { Value v; v.t = V_NULL; v.as.i = 0; return v; }
static Value vInt(int i) { Value v; v.t = V_INT; v.as.i = i; return v; }
static Value vBool(bool b) { Value v; v.t = V_BOOL; v.as.i = b; return v; }
static Value vFloat(float f) { Value v; v.t = V_FLOAT; v.as.f = f; return v; }
static const char* cstr(Value v) { return v.t == V_OBJ && v.as.o && v.as.o->type == O_STRING ? ((ObjString*)v.as.o)->chars : ""; }
static Value vStr(VM* m, const char* s) { return vm_make_string(m, s, (int)strlen(s)); }
static bool okObj(VM* m, int i)
{
	if (i >= 0 && i < nobj) return true;
	ms_vm_fail(m, "no object %d", i);
	return false;
}
static void setFlag(int i, u16 f, bool on)
{
	u16 old = objs[i].flags;
	objs[i].flags = on ? old | f : old & ~f;
	if (objs[i].flags != old) objs[i].flags |= OF_CHANGED;
}

// "... line 12 ..." of the glued source -> "... beczka.cs:3 ..."
static void fixLines(char* msg, int cap)
{
	char* p = strstr(msg, "line ");
	if (!p || !nfiles) return;
	int line = atoi(p + 5);
	if (line <= 0) return;
	int f = 0;
	while (f + 1 < nfiles && fileLine[f + 1] <= line) f++;
	char rest[256];
	char* q = p + 5;
	while (*q >= '0' && *q <= '9') q++;
	snprintf(rest, sizeof rest, "%s", q);
	snprintf(p, cap - (p - msg), "%s:%d%s", fileName[f], line - fileLine[f] + 1, rest);
}

// ------------------------------------------------------------------ Game.*

static Value g_message(VM* m, Value* a) { (void)m; gameMessage(cstr(a[0])); return vNull(); }
static Value g_log(VM* m, Value* a) { (void)m; gameLogLine(cstr(a[0])); return vNull(); }
static Value g_name(VM* m, Value* a) { return okObj(m, a[0].as.i) ? vStr(m, objName(a[0].as.i)) : vNull(); }
static Value g_id(VM* m, Value* a) { return okObj(m, a[0].as.i) ? vStr(m, objId(a[0].as.i)) : vNull(); }
static Value g_type(VM* m, Value* a) { return okObj(m, a[0].as.i) ? vInt(objType(a[0].as.i)) : vInt(0); }
static Value g_find(VM* m, Value* a) { (void)m; return vInt(objFind(cstr(a[0]), -1)); }
static Value g_findNext(VM* m, Value* a) { (void)m; return vInt(objFind(cstr(a[0]), a[1].as.i)); }
static Value g_isOpen(VM* m, Value* a) { return vBool(okObj(m, a[0].as.i) && (objs[a[0].as.i].flags & OF_OPEN)); }
static Value g_isTaken(VM* m, Value* a) { return vBool(okObj(m, a[0].as.i) && (objs[a[0].as.i].flags & OF_TAKEN)); }
static Value g_isLocked(VM* m, Value* a) { return vBool(okObj(m, a[0].as.i) && (objs[a[0].as.i].flags & OF_LOCKED)); }
static Value g_setOpen(VM* m, Value* a) { if (okObj(m, a[0].as.i)) setFlag(a[0].as.i, OF_OPEN, a[1].as.i); return vNull(); }
static Value g_setTaken(VM* m, Value* a) { if (okObj(m, a[0].as.i)) setFlag(a[0].as.i, OF_TAKEN, a[1].as.i); return vNull(); }
static Value g_setLocked(VM* m, Value* a) { if (okObj(m, a[0].as.i)) setFlag(a[0].as.i, OF_LOCKED, a[1].as.i); return vNull(); }
static Value g_bag(VM* m, Value* a)
{
	(void)m; (void)a;
	int n = 0;
	for (int i = 0; i < nobj; i++) if ((objs[i].flags & OF_TAKEN) && objType(i) == OT_ITEM) n++;
	return vInt(n);
}
static Value g_time(VM* m, Value* a) { (void)m; (void)a; return vFloat(osGetTime() / 1000.0f); }
static Value g_distance(VM* m, Value* a)
{
	if (!okObj(m, a[0].as.i)) return vFloat(0);
	float f[3];
	gameFeet(f);
	const float* p = objs[a[0].as.i].pos;
	return vFloat(sqrtf((p[0] - f[0]) * (p[0] - f[0]) + (p[1] - f[1]) * (p[1] - f[1]) + (p[2] - f[2]) * (p[2] - f[2])));
}

static const NativeDef natives[] = {
	{ "Game", "Message", 1, { T_STRING }, T_VOID, g_message },
	{ "Game", "Log", 1, { T_STRING }, T_VOID, g_log },
	{ "Game", "Name", 1, { T_INT }, T_STRING, g_name },
	{ "Game", "Id", 1, { T_INT }, T_STRING, g_id },
	{ "Game", "Type", 1, { T_INT }, T_INT, g_type },
	{ "Game", "Find", 1, { T_STRING }, T_INT, g_find },
	{ "Game", "FindNext", 2, { T_STRING, T_INT }, T_INT, g_findNext },
	{ "Game", "IsOpen", 1, { T_INT }, T_BOOL, g_isOpen },
	{ "Game", "IsTaken", 1, { T_INT }, T_BOOL, g_isTaken },
	{ "Game", "IsLocked", 1, { T_INT }, T_BOOL, g_isLocked },
	{ "Game", "SetOpen", 2, { T_INT, T_BOOL }, T_VOID, g_setOpen },
	{ "Game", "SetTaken", 2, { T_INT, T_BOOL }, T_VOID, g_setTaken },
	{ "Game", "SetLocked", 2, { T_INT, T_BOOL }, T_VOID, g_setLocked },
	{ "Game", "Bag", 0, { 0 }, T_INT, g_bag },
	{ "Game", "Time", 0, { 0 }, T_FLOAT, g_time },
	{ "Game", "Distance", 1, { T_INT }, T_FLOAT, g_distance },
};
static const ConstDef consts[] = {
	{ "Game", "Item", 0, OT_ITEM, 0 }, { "Game", "Door", 0, OT_DOOR, 0 }, { "Game", "Container", 0, OT_CONT, 0 },
};
static const char* const BASE_SRC =
	"public class ObjectScript {\n"
	"    public int Obj;                                     // the object this copy belongs to\n"
	"    public virtual string Dla() { return \"\"; }       // what it belongs to: \"barrel_*, crate_*\", #drzwi...\n"
	"    public virtual bool OnActivate(int obj) { return true; }   // false: the default (take / open) does not happen\n"
	"    public virtual void OnTake(int obj) { }\n"
	"    public virtual void OnOpen(int obj) { }\n"
	"    public virtual void OnClose(int obj) { }\n"
	"}\n";

// ------------------------------------------------------------------ loading

static int millis(void) { return (int)osGetTime(); }
// a script that runs longer than 100 ms in one call (a loop that never ends) is stopped
static int interrupt(void) { return (svcGetSystemTick() - callStart) / CPU_TICKS_PER_MSEC > 100; }
static void output(const char* s)
{
	char b[160];
	snprintf(b, sizeof b, "%s", s);
	b[strcspn(b, "\r\n")] = 0;
	if (b[0]) gameMessage(b);
}

static void failed(const char* where)
{
	snprintf(err, sizeof err, "%s: %s", where, ms_vm_error(vm));
	fixLines(err, sizeof err);
	gameLogLine(err);
	gameMessage(err);
}

static int byName(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

// every *.cs of the folder, by name, glued (a newline between)
static char* readScripts(const char* dir)
{
	DIR* d = opendir(dir);
	if (!d) return NULL;
	struct dirent* e;
	nfiles = 0;
	while ((e = readdir(d)) && nfiles < MAXFILES) {
		size_t n = strlen(e->d_name);
		if (n > 3 && n < sizeof fileName[0] && !strcmp(e->d_name + n - 3, ".cs")) snprintf(fileName[nfiles++], sizeof fileName[0], "%s", e->d_name);
	}
	closedir(d);
	qsort(fileName, nfiles, sizeof fileName[0], byName);
	char* src = NULL;
	size_t len = 0;
	int line = 1;
	for (int i = 0; i < nfiles; i++) {
		char path[128];
		snprintf(path, sizeof path, "%s/%s", dir, fileName[i]);
		FILE* f = fopen(path, "rb");
		if (!f) continue;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		char* grown = (char*)realloc(src, len + n + 2);
		if (!grown) { fclose(f); break; }
		src = grown;
		size_t got = fread(src + len, 1, n, f);
		fclose(f);
		if (got >= 3 && (u8)src[len] == 0xEF && (u8)src[len + 1] == 0xBB && (u8)src[len + 2] == 0xBF) {   // a BOM: out
			memmove(src + len, src + len + 3, got - 3);
			got -= 3;
		}
		fileLine[i] = line;
		for (size_t k = 0; k < got; k++) if (src[len + k] == '\n') line++;
		len += got;
		src[len++] = '\n';
		line++;
		src[len] = 0;
	}
	return src;
}

// every class derived from ObjectScript: what it belongs to (a copy made just to ask Dla()), its events
static void findScripts(void)
{
	int base = ms_class_find(prog, "ObjectScript");
	if (base < 0) return;
	fieldObj = ms_field_find(prog, base, "Obj");
	for (int c = 0; c < ms_class_count(prog) && nscr < MAXSCR; c++) {
		if (c == base || !ms_class_is(prog, c, base) || !ms_class_can_create(prog, c)) continue;
		Value probe, res = vNull();
		int dla = ms_method_find(prog, c, "Dla", 0, NULL);
		callStart = svcGetSystemTick();
		if (!vm_new_object(vm, c, &probe) || dla < 0 || vm_call_args(vm, dla, &probe, 1, &res) != 1) {
			failed(ms_class_name(prog, c));
			continue;
		}
		Script* s = &scr[nscr];
		memset(s, 0, sizeof *s);
		s->cls = c;
		// "barrel_*, crate_*" -> the patterns (split at the commas)
		const char* p = cstr(res);
		while (*p && s->npat < MAXPAT) {
			while (*p == ' ' || *p == ',') p++;
			int n = 0;
			while (p[n] && p[n] != ',') n++;
			int m = n;
			while (m > 0 && p[m - 1] == ' ') m--;   // (spaces inside stay: ids have them)
			if (m > 0 && m < (int)sizeof s->pat[0]) { memcpy(s->pat[s->npat], p, m); s->pat[s->npat][m] = 0; s->npat++; }
			p += n;
		}
		if (!s->npat) continue;   // a helper class, not for any object
		for (int e = 0; e < NEV; e++) {
			int owner = -1;
			s->fn[e] = ms_method_find(prog, c, EVNAME[e], 1, &owner);
			if (owner == base) s->fn[e] = -1;
		}
		char b[160];
		snprintf(b, sizeof b, "script %s -> %s%s%s", ms_class_name(prog, c), cstr(res), s->fn[EV_ACTIVATE] >= 0 ? " OnActivate" : "",
		         s->fn[EV_TAKE] >= 0 || s->fn[EV_OPEN] >= 0 || s->fn[EV_CLOSE] >= 0 ? " +after" : "");
		gameLogLine(b);
		nscr++;
	}
}

bool scriptInit(void)
{
	static bool registered;
	if (!registered) {   // (the language's tables once; a reload only compiles again)
		registered = true;
		ms_set_clock(millis);
		ms_set_interrupt(interrupt);
		lib_set_output(output);
		lib_register_console();
		lib_register_math();
		lib_register_random();
		lib_register_strings();
		lib_register_convert();
		MS_REGISTER(natives);
		MS_REGISTER_CONSTS(consts);
		ms_register_source(BASE_SRC);
		ms_register_hook("Update");
	}
	// the old program out (a reload): its copies of the scripts go with it, the objects' state stays
	if (vm) { vm_free(vm); vm = NULL; }
	if (prog) { ms_free_program(prog); prog = NULL; }
	nscr = 0; ninst = 0; fnUpdate = -1;
	// the SD card first (edit, upload by FTP, L+X in the game: no build), else the ones built in
	const char* dir = SD_SCRIPTS;
	char* src = readScripts(dir);
	if (!src || !nfiles) { free(src); dir = "romfs:/scripts"; src = readScripts(dir); }
	if (!src) { snprintf(err, sizeof err, "no %s/*.cs nor romfs:/scripts/*.cs", SD_SCRIPTS); return false; }
	u64 t0 = svcGetSystemTick();
	prog = ms_compile(src, err, sizeof err);
	free(src);
	if (!prog) { fixLines(err, sizeof err); gameLogLine(err); return false; }
	vm = vm_new(prog);
	char b[128];
	snprintf(b, sizeof b, "scripts: %d files from %s compiled in %.1f ms", nfiles, dir, (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC);
	gameLogLine(b);
	callStart = svcGetSystemTick();
	int si = ms_entry(prog, MS_ENTRY_STATIC_INIT), mi = ms_entry(prog, MS_ENTRY_MAIN);
	if (si >= 0 && vm_call(vm, si) != 1) { failed("static init"); return false; }
	if (mi >= 0 && vm_call(vm, mi) != 1) { failed("Main"); return false; }   // (optional)
	findScripts();
	fnUpdate = ms_hook(prog, "Update");
	snprintf(b, sizeof b, "scripts: %d object scripts, Update %s", nscr, fnUpdate >= 0 ? "yes" : "no");
	gameLogLine(b);
	snprintf(scriptSource, sizeof scriptSource, "%s", dir);
	err[0] = 0;
	return true;
}

// ------------------------------------------------------------------ events

static bool matches(const Script* s, int obj)
{
	if (s->broken) return false;
	const char* id = objId(obj);
	int ty = objType(obj);
	for (int k = 0; k < s->npat; k++) {
		const char* p = s->pat[k];
		if (p[0] == '#') {
			if ((ty == OT_DOOR && !strcmp(p, "#drzwi")) || (ty == OT_ITEM && !strcmp(p, "#przedmiot")) ||
			    (ty == OT_CONT && !strcmp(p, "#pojemnik"))) return true;
			continue;
		}
		size_t n = strlen(p);
		if (n && p[n - 1] == '*' ? !strncmp(id, p, n - 1) : !strcmp(id, p)) return true;
	}
	return false;
}

// this object's copy of script k, made the first time (-1: no room / an error)
static int instanceOf(int k, int obj)
{
	for (int i = 0; i < ninst; i++) if (inst[i].obj == obj && inst[i].script == k) return inst[i].handle;
	if (ninst >= MAXINST) return -1;
	Value v;
	callStart = svcGetSystemTick();
	if (!vm_new_object(vm, scr[k].cls, &v)) { failed(ms_class_name(prog, scr[k].cls)); return -1; }
	if (fieldObj >= 0) ms_set_field(vm, v, fieldObj, vInt(obj));
	int h = ms_vm_keep(vm, v);
	if (h < 0) return -1;
	inst[ninst].obj = obj; inst[ninst].script = k; inst[ninst].handle = h;
	ninst++;
	return h;
}

// calls event e of every script that belongs to obj; *allow &= what OnActivate returned
static void fire(int obj, int e, bool* allow)
{
	if (!vm) return;
	u64 t0 = svcGetSystemTick();
	for (int k = 0; k < nscr; k++) {
		Script* s = &scr[k];
		if (s->fn[e] < 0 || !matches(s, obj)) continue;
		int h = instanceOf(k, obj);
		if (h < 0) continue;
		Value args[2] = { ms_vm_kept(vm, h), vInt(obj) }, res = vNull();
		callStart = svcGetSystemTick();
		scriptCalls++;
		if (vm_call_args(vm, s->fn[e], args, 2, &res) != 1) {
			s->broken = true;
			char where[64];
			snprintf(where, sizeof where, "%s.%s", ms_class_name(prog, s->cls), EVNAME[e]);
			failed(where);
			continue;
		}
		if (allow && res.t == V_BOOL && !res.as.i) *allow = false;
	}
	scriptMs += (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC;
}

bool scriptActivate(int obj)
{
	bool allow = true;
	fire(obj, EV_ACTIVATE, &allow);
	return allow;
}

void scriptAfter(int obj, int event)
{
	fire(obj, event == SE_TAKE ? EV_TAKE : event == SE_OPEN ? EV_OPEN : EV_CLOSE, NULL);
}

// Nothing per frame unless a script asks for it: only a static Update() (it then also gets the coroutines
// ticked). The object scripts run on events alone.
void scriptFrame(void)
{
	if (!vm || fnUpdate < 0) return;
	u64 t0 = svcGetSystemTick();
	callStart = t0;
	if (vm_call(vm, fnUpdate) != 1) { failed("Update"); fnUpdate = -1; }
	callStart = svcGetSystemTick();
	if (fnUpdate >= 0 && vm_tick_coroutines(vm) == 0) failed("coroutine");
	scriptMs += (svcGetSystemTick() - t0) / (float)CPU_TICKS_PER_MSEC;
}
