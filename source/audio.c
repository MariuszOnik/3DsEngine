// Sound (ndsp: the DSP mixes, the CPU only feeds it). tools/sndbake.py made raw 16-bit PCM at
// 22050 Hz: the effects (mono) are loaded whole into linear memory, the music (stereo) is streamed
// from romfs by a thread of its own in 4 buffers of 0.37 s.
//
// Needs the DSP's firmware, sdmc:/3ds/dspfirm.cdc (made once by DSP1.3dsx); without it there is
// no sound and the rest goes on.
//
// Channels: 0 the music, 1-5 the effects (round robin), 6 and 7 loops (the torch, under water).

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"

#define RATE 22050
#define MUS_FRAMES 8192            // per buffer (stereo frames)
#define MUS_BUFS 4

static const char* const NAMES[SND_COUNT] = {
	"walkl_lt", "walkr_lt", "runl_lt", "runr_lt", "walkl_md", "walkr_md", "runl_md", "runr_md",
	"walkl_hv", "walkr_hv", "runl_hv", "runr_hv", "land", "swiml", "swimr", "splash",
	"swing1", "swing2", "bladeout", "clothes", "armour", "click", "torch", "torchout", "underwater",
	"hitbody", "hitarmour", "miss", "punch", "fall",
};

static bool ok;
static s16* sfx[SND_COUNT];
static u32 sfxLen[SND_COUNT];      // samples
static ndspWaveBuf sfxBuf[8];      // one per channel
static int nextCh = 1;

static FILE* musF;
static s16* musMem;
static ndspWaveBuf musBuf[MUS_BUFS];
static Thread musThread;
static volatile bool musQuit;
static float musVol = 0.55f, musWant = 0.55f;

bool audioOk(void) { return ok; }

static s16* loadRaw(const char* name, u32* samples)
{
	char p[64];
	snprintf(p, sizeof p, "romfs:/snd/%s.raw", name);
	FILE* f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	s16* d = (s16*)linearAlloc(n);
	if (d && fread(d, 1, n, f) != (size_t)n) { linearFree(d); d = NULL; }
	fclose(f);
	if (d) { DSP_FlushDataCache(d, n); *samples = n / 2; }
	return d;
}

// The music: whenever a buffer is played out, the next piece of the file into it (from the start
// again at the end).
static void fill(ndspWaveBuf* b)
{
	s16* d = (s16*)b->data_vaddr;
	size_t got = fread(d, 4, MUS_FRAMES, musF);
	if (got < MUS_FRAMES) {
		fseek(musF, 0, SEEK_SET);
		got += fread(d + got * 2, 4, MUS_FRAMES - got, musF);
	}
	b->nsamples = got;
	DSP_FlushDataCache(d, MUS_FRAMES * 4);
	ndspChnWaveBufAdd(0, b);
}

static void musMain(void* arg)
{
	(void)arg;
	while (!musQuit) {
		for (int i = 0; i < MUS_BUFS; i++)
			if (musBuf[i].status == NDSP_WBUF_DONE) fill(&musBuf[i]);
		// the volume eased towards what is wanted (under the water: lower)
		musVol += (musWant - musVol) * 0.1f;
		float mix[12] = { musVol, musVol };
		ndspChnSetMix(0, mix);
		svcSleepThread(30 * 1000000LL);
	}
}

bool audioInit(void)
{
	if (R_FAILED(ndspInit())) return false;
	ok = true;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	for (int c = 0; c < 8; c++) {
		ndspChnReset(c);
		ndspChnSetInterp(c, NDSP_INTERP_LINEAR);
		ndspChnSetRate(c, RATE);
		ndspChnSetFormat(c, c == 0 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	}
	for (int i = 0; i < SND_COUNT; i++) sfx[i] = loadRaw(NAMES[i], &sfxLen[i]);
	musF = fopen("romfs:/snd/music.raw", "rb");
	musMem = musF ? (s16*)linearAlloc(MUS_FRAMES * 4 * MUS_BUFS) : NULL;
	if (musF && musMem) {
		memset(musBuf, 0, sizeof musBuf);
		for (int i = 0; i < MUS_BUFS; i++) {
			musBuf[i].data_vaddr = musMem + i * MUS_FRAMES * 2;
			fill(&musBuf[i]);
		}
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		musThread = threadCreate(musMain, NULL, 16 * 1024, prio - 2, -2, false);
	}
	return true;
}

void audioExit(void)
{
	if (!ok) return;
	if (musThread) { musQuit = true; threadJoin(musThread, U64_MAX); threadFree(musThread); }
	ndspExit();
	if (musF) fclose(musF);
	if (musMem) linearFree(musMem);
	for (int i = 0; i < SND_COUNT; i++) if (sfx[i]) linearFree(sfx[i]);
	ok = false;
}

void audioPlay(int s, float vol, float pan)
{
	if (!ok || s < 0 || s >= SND_COUNT || !sfx[s]) return;
	// a free channel (not one cleared now: the DSP clears it at its next frame and would take the
	// new buffer with it, so the effects were silent); all busy: this one is left out
	int c = -1;
	for (int k = 0; k < 5 && c < 0; k++) {
		int t = nextCh;
		nextCh = nextCh >= 5 ? 1 : nextCh + 1;
		if (!ndspChnIsPlaying(t) && sfxBuf[t].status != NDSP_WBUF_QUEUED && sfxBuf[t].status != NDSP_WBUF_PLAYING) c = t;
	}
	if (c < 0) return;
	float l = vol * (pan <= 0 ? 1.0f : 1.0f - pan), r = vol * (pan >= 0 ? 1.0f : 1.0f + pan);
	float mix[12] = { l, r };
	ndspChnSetMix(c, mix);
	memset(&sfxBuf[c], 0, sizeof sfxBuf[c]);
	sfxBuf[c].data_vaddr = sfx[s];
	sfxBuf[c].nsamples = sfxLen[s];
	ndspChnWaveBufAdd(c, &sfxBuf[c]);
}

void audioLoop(int ch, int s, float vol)
{
	if (!ok || ch < 6 || ch > 7) return;
	bool playing = ndspChnIsPlaying(ch);
	if (vol <= 0.001f || s < 0 || !sfx[s]) {
		if (playing) ndspChnWaveBufClear(ch);
		return;
	}
	float mix[12] = { vol, vol };
	ndspChnSetMix(ch, mix);
	if (!playing) {
		memset(&sfxBuf[ch], 0, sizeof sfxBuf[ch]);
		sfxBuf[ch].data_vaddr = sfx[s];
		sfxBuf[ch].nsamples = sfxLen[s];
		sfxBuf[ch].looping = true;
		ndspChnWaveBufAdd(ch, &sfxBuf[ch]);
	}
}

void audioMusicVolume(float v) { musWant = v; }
