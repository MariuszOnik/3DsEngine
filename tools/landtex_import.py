"""Land textures from the player's own Morrowind copy -> PNG for the on-console editor (sdmc:/3ds/mwedit/textures/).

Takes the land textures (LTEX) the exterior cells use most, reads them from the BSA (DDS) and writes 256 x 256 PNG
files named as in Morrowind (tx_ai_grass_01.png): OpenMW finds the same names in the player's data (.dds in place
of .png), so a world painted with them opens there with the original textures.

    python tools/landtex_import.py --count 16 --out <folder>
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mwbake  # noqa: E402  (BSA / ESM / texture reading)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=r"D:\Programowanie\OpenMorrowind\data\Data Files")
    ap.add_argument("--count", type=int, default=16, help="how many of the most used land textures")
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    vfs = mwbake.VFS(a.data, ["Morrowind.bsa"])
    esm = mwbake.ESM()
    esm.load(os.path.join(a.data, "Morrowind.esm"))
    use = collections.Counter()
    for land in esm.lands.values():
        t = land.get("t") if isinstance(land, dict) else None
        if t is None:
            continue
        for v in t.ravel():
            if v:
                use[int(v) - 1] += 1
    tex = mwbake.TexStore(vfs, a.size)
    os.makedirs(a.out, exist_ok=True)
    done = 0
    for idx, n in use.most_common():
        name = esm.ltex.get(idx)
        if not name:
            continue
        img = tex.image(name)
        if img is None:
            print("  missing", name)
            continue
        base = os.path.splitext(os.path.basename(name.replace("\\", "/")))[0].lower()
        img = img.convert("RGB")
        if img.size != (a.size, a.size):
            img = img.resize((a.size, a.size), mwbake.Image.LANCZOS)
        img.save(os.path.join(a.out, base + ".png"), optimize=True)
        print("%-28s used in %5d squares" % (base + ".png", n))
        done += 1
        if done >= a.count:
            break


if __name__ == "__main__":
    main()
