// Scripts in MiniSharp2 (E:\MiniSharp2, docs/SKRYPTY.md): romfs:/scripts/*.cs compiled at the start.
// A script is a class derived from ObjectScript, attached in Main to objects by id, id prefix or type:
//   Game.Attach("barrel_*", new Beczka());  Game.AttachType(Game.Door, new Drzwi());
// The engine calls it on events (the player uses / takes / opens / closes an object); the script answers
// through Game.* (messages, the objects' state). Events, not polling: a script costs nothing until something happens.
#pragma once
#include <stdbool.h>

enum { SE_TAKE, SE_OPEN, SE_CLOSE };

#define SD_SCRIPTS "sdmc:/3ds/mwcell3d/scripts"
// Compiles the scripts: from SD_SCRIPTS when it has *.cs files (no build needed: upload by FTP, L+X in the game
// reloads), else romfs:/scripts. Called again = a reload (the scripts' fields start anew, the objects keep their
// state). false: no scripts or a compile error (scriptError says which, with file:line).
bool scriptInit(void);
extern char scriptSource[48];           // where they came from
const char* scriptError(void);
bool scriptActivate(int obj);           // before the default action: false = a script stopped it
void scriptAfter(int obj, int event);   // after the default action (SE_*)
void scriptFrame(void);                 // Update() of the scripts that have one, their coroutines
extern float scriptMs;                  // the scripts' CPU time in the last frame (events + Update)
extern int scriptCalls;

// the engine's side (main.c)
void gameMessage(const char* text);     // a line on the bottom screen for a few seconds (and the log)
void gameLogLine(const char* text);
