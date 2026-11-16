"""The bottom screen's interface from Morrowind's own: the window and button frames
(textures/menu_*.dds), the "equipped" backing and the items' icons (Icons/, the ITEX of each
ARMO / CLOT / WEAP / LIGH record), packed by tex3ds into one atlas for citro2d.

romfs/ui.t3x (the atlas) and romfs/ui.txt (the pieces' names, one per line, in the atlas' order).
The inventory's icons are named as main.c asks for them: outfit<N>, helmet, hands, weapon<N>, torch.
Called by mwbake.py after the character (it needs its outfits and weapons).
"""
import os, struct, subprocess, tempfile, io
from PIL import Image

import mwbake as M

TEX3DS = r"C:\devkitPro\tools\bin\tex3ds.exe"
FRAMES = {
    "thick": "menu_thick_border",
    "button": "menu_button_frame",
}
PARTS = ["top", "bottom", "left", "right", "top_left_corner", "top_right_corner",
         "bottom_left_corner", "bottom_right_corner"]
EXTRA = ["menu_icon_equip", "menu_icon_none", "menu_icon_magic_mini", "menu_bar_red"]


def icons_of(esm_path):
    """record id -> icon path (ITEX), for ARMO / CLOT / WEAP / LIGH"""
    data = open(esm_path, "rb").read()
    out = {}
    p = 0
    while p + 16 <= len(data):
        tag = data[p:p + 4]
        size = struct.unpack_from("<I", data, p + 4)[0]
        body = data[p + 16:p + 16 + size]
        p += 16 + size
        if tag in (b"ARMO", b"CLOT", b"WEAP", b"LIGH", b"MISC"):
            rid = icon = None
            for n, v in M.subrecords(body):
                if n == b"NAME": rid = M.cstr(v).lower()
                elif n == b"ITEX": icon = M.cstr(v)
            if rid and icon:
                out[rid] = icon
    return out


def load_img(vfs, path):
    base = os.path.splitext(path.lower().replace("/", "\\"))[0]
    for ext in (".dds", ".tga", ".bmp"):
        d = vfs.read(base + ext)
        if d:
            try:
                im = Image.open(io.BytesIO(d)); im.load()
                return im.convert("RGBA")
            except Exception:
                pass
    return None


def bake(vfs, esm_path, ch, out, log=print):
    icons = icons_of(esm_path)
    pieces = []   # (name, image)

    def icon(name, rid=None, path=None):
        im = None
        if rid and rid in icons:
            im = load_img(vfs, "icons\\" + icons[rid])
        if im is None and path:
            im = load_img(vfs, path)
        if im is None:
            im = load_img(vfs, "icons\\default icon")
        if im is None:
            log("  ui: no icon for " + name); return
        if im.size != (32, 32):
            im = im.resize((32, 32), Image.LANCZOS)
        pieces.append((name, im))

    for f, base in FRAMES.items():
        for part in PARTS:
            im = load_img(vfs, "textures\\%s_%s" % (base, part))
            if im is not None:
                pieces.append(("%s_%s" % (f, part), im))
    for e in EXTRA:
        im = load_img(vfs, "textures\\" + e)
        if im is not None:
            pieces.append((e, im))
    # the inventory: per outfit the icon of what it wears on the chest
    for k, (oname, lst) in enumerate(ch.outfits):
        rid = None
        for ii in lst:
            iid, helmet, parts = ch.items[ii]
            if any(s == 3 for s, g in parts): rid = iid; break
        if rid is None and lst: rid = ch.items[lst[0]][0]
        icon("outfit%d" % k, rid, None if rid else "icons\\default icon")
    icon("helmet", "iron_helmet")
    for k, (iid, helmet, parts) in enumerate(ch.items):   # every piece on its own
        icon("item%d" % k, iid)
    icon("hands", None, "icons\\handtohand")
    for k, (wid, g) in enumerate(ch.weapons):
        icon("weapon%d" % k, wid)
    icon("torch", "light_com_torch_01")
    # the atlas
    tmp = tempfile.mkdtemp()
    files = []
    for i, (name, im) in enumerate(pieces):
        fn = os.path.join(tmp, "%03d.png" % i)
        im.save(fn); files.append(fn)
    t3x = os.path.join(out, "ui.t3x")
    r = subprocess.run([TEX3DS, "--atlas", "-f", "rgba8", "-z", "none", "-o", t3x] + files, capture_output=True, text=True)
    if r.returncode:
        log("  ui: tex3ds failed: " + r.stderr[-300:]); return
    with open(os.path.join(out, "ui.txt"), "w") as f:
        for name, im in pieces:
            f.write(name + "\n")
    log("ui: %d pieces, atlas %.0f KB" % (len(pieces), os.path.getsize(t3x) / 1024))


if __name__ == "__main__":
    import chrbake
    data = r"D:\Programowanie\OpenMorrowind\data\Data Files"
    vfs = M.VFS(data, ["Morrowind.bsa"])
    tex = M.TexStore(vfs, 128)
    ch = chrbake.Character(vfs, os.path.join(data, "Morrowind.esm"), tex, log=lambda *a: None)
    bake(vfs, os.path.join(data, "Morrowind.esm"), ch, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "romfs"))
