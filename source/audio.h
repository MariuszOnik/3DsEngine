#pragma once
#include <stdbool.h>

// the effects, in the order of audio.c's NAMES (tools/sndbake.py SOUNDS)
enum {
	SND_WALKL_LT, SND_WALKR_LT, SND_RUNL_LT, SND_RUNR_LT, SND_WALKL_MD, SND_WALKR_MD, SND_RUNL_MD, SND_RUNR_MD,
	SND_WALKL_HV, SND_WALKR_HV, SND_RUNL_HV, SND_RUNR_HV, SND_LAND, SND_SWIML, SND_SWIMR, SND_SPLASH,
	SND_SWING1, SND_SWING2, SND_BLADEOUT, SND_CLOTHES, SND_ARMOUR, SND_CLICK, SND_TORCH, SND_TORCHOUT,
	SND_UNDERWATER, SND_HITBODY, SND_HITARMOUR, SND_MISS, SND_PUNCH, SND_FALL, SND_COUNT
};

bool audioInit(void);           // false: no DSP firmware (sdmc:/3ds/dspfirm.cdc), silent
bool audioOk(void);
void audioExit(void);
void audioPlay(int snd, float vol, float pan);   // pan -1 left .. 1 right
void audioLoop(int ch, int snd, float vol);      // channels 6, 7: a looping sound, vol 0 stops it
void audioMusicVolume(float v);
