"""Sounds and music for the 3DS: Morrowind's WAV / MP3 files decoded (miniaudio) into raw signed
16-bit little-endian PCM at 22050 Hz, played by the DSP (ndsp) without decoding on the console.
Effects mono, loaded whole; the music stereo, streamed from romfs (88 KB/s).

romfs/snd/<name>.raw for each SOUNDS entry, romfs/snd/music.raw. The names are the ones main.c plays.
Usage: python sndbake.py [--data "<Data Files>"] [--out romfs]  (mwbake.py calls bake() too)
"""
import argparse, os, sys
import numpy as np
import miniaudio

RATE = 22050
# name on the 3DS -> file under Data Files/Sound
SOUNDS = {
    "walkl_lt": "Fx/FOOT/walkl_lt.wav", "walkr_lt": "Fx/FOOT/walkr_lt.wav",
    "runl_lt": "Fx/FOOT/runl_lt.wav", "runr_lt": "Fx/FOOT/runr_lt.wav",
    "walkl_md": "Fx/FOOT/walkl_md.wav", "walkr_md": "Fx/FOOT/walkr_md.wav",
    "runl_md": "Fx/FOOT/runl_md.wav", "runr_md": "Fx/FOOT/runr_md.wav",
    "walkl_hv": "Fx/FOOT/walkl_hv.wav", "walkr_hv": "Fx/FOOT/walkr_hv.wav",
    "runl_hv": "Fx/FOOT/runl_hv.wav", "runr_hv": "Fx/FOOT/runr_hv.wav",
    "land": "Fx/FOOT/land_md.wav",
    "swiml": "Fx/FOOT/swimLEFT.wav", "swimr": "Fx/FOOT/swimRIGHT.wav",
    "splash": "Fx/FOOT/splsh.wav",
    "swing1": "Fx/swoosh 1.wav", "swing2": "Fx/swoosh 2.wav",
    "bladeout": "Fx/item/longbladOUT.wav", "clothes": "Fx/item/clothes.wav", "armour": "Fx/item/armrmed.wav",
    "click": "Fx/menu click.wav",
    "torch": "Fx/envrn/fire_trch.wav", "torchout": "Fx/envrn/trch_out.wav",
    "underwater": "Fx/envrn/undrwatr.wav",
    "hitbody": "Fx/body hit.wav", "hitarmour": "Fx/Light Armor Hit.wav", "miss": "Fx/miss.wav",
    "punch": "Fx/punch 1.wav", "fall": "Fx/bodyfallMED.wav",
}
MUSIC = "Music/Explore/mx_explore_1.mp3"


def decode(path, channels):
    d = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16, nchannels=channels,
                              sample_rate=RATE)
    return np.frombuffer(d.samples, dtype="<i2")


def bake(data, out, log=print):
    os.makedirs(os.path.join(out, "snd"), exist_ok=True)
    total = 0
    for name, rel in SOUNDS.items():
        p = os.path.join(data, "Sound", rel)
        if not os.path.exists(p):
            log("  sound missing: " + rel); continue
        a = decode(p, 1)
        open(os.path.join(out, "snd", name + ".raw"), "wb").write(a.tobytes())
        total += a.nbytes
    p = os.path.join(data, MUSIC)
    if os.path.exists(p):
        m = decode(p, 2)
        open(os.path.join(out, "snd", "music.raw"), "wb").write(m.tobytes())
        log("sounds: %d effects %.0f KB, music %.0f s %.1f MB" % (len(SOUNDS), total / 1024, len(m) / 2 / RATE, m.nbytes / 1048576))
    else:
        log("  music missing: " + MUSIC)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=r"D:\Programowanie\OpenMorrowind\data\Data Files")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "romfs"))
    a = ap.parse_args()
    bake(a.data, a.out)
