#!/usr/bin/env python3
"""Bakes Morrowind exterior cells into a compact format for the 3DS test engine.

Reads Morrowind.esm (CELL refs, LAND, LTEX, object records), the BSA archives and the
loose Data Files, parses NIF 4.0.0.2 meshes (static parts only) and writes into romfs/:

  index.bin          "MWI2", u32 count, count x (i32 gx, i32 gy, u32 file size), f32 start x y z yaw,
                     u32 route points, points x (f32 x y z): the benchmark walk
  textures.bin       "MWT1", u32 count, per texture: u16 w, u16 h, u8 fmt (0 RGB565, 1 RGBA4),
                     u8 levels, u16 0, u32 bytes, data (all mipmap levels, 3DS tiled)
  cells/<gx>_<gy>.bin one cell: terrain (65x65 heights, composite texture) + every static
                     object of the cell pre-transformed and merged by material, per 2x2 sub-chunk

Like Gothic3DS: the light is baked into the vertex colours on the PC, positions are int16
relative to the cell centre, one draw call per material per sub-chunk.

Usage: python mwbake.py [--data "<Data Files>"] [--center -2 -9] [--radius 2] [--tex 128]
                        [--terrain-tex 256] [--out ../romfs]
"""
import argparse, math, os, struct, sys, time, io
import numpy as np
from PIL import Image

CELL = 8192.0
HALF = CELL / 2
UV_SCALE = 1024.0  # uv as int16 / 1024 (range +-32)
SMALL_RADIUS = 180.0  # objects smaller than this (grass 35-104, kelp 89-160, shells; trees and rocks 250+): batch flag 8
SUN = np.array([-0.35, 0.45, 0.82]); SUN /= np.linalg.norm(SUN)
AMBIENT = np.array([0.52, 0.54, 0.60])
SUNCOL = np.array([0.62, 0.58, 0.50])

# --------------------------------------------------------------------------- VFS

class BSA:
    def __init__(self, path):
        self.path = path
        self.files = {}
        with open(path, "rb") as f:
            ver, hashofs, n = struct.unpack("<III", f.read(12))
            if ver != 0x100:
                raise ValueError("not a Morrowind BSA: " + path)
            recs = struct.unpack("<%dI" % (2 * n), f.read(8 * n))
            nameofs = struct.unpack("<%dI" % n, f.read(4 * n))
            names = f.read(hashofs - 12 * n)
            data0 = 12 + hashofs + 8 * n
            for i in range(n):
                s = nameofs[i]
                e = names.index(b"\0", s)
                name = names[s:e].decode("latin-1").lower().replace("/", "\\")
                self.files[name] = (data0 + recs[2 * i + 1], recs[2 * i])

    def read(self, name):
        off, size = self.files[name]
        with open(self.path, "rb") as f:
            f.seek(off)
            return f.read(size)


class VFS:
    def __init__(self, data_dir, archives):
        self.dir = data_dir
        self.loose = {}
        for root, _, files in os.walk(data_dir):
            for fn in files:
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, data_dir).lower().replace("/", "\\")
                self.loose[rel] = full
        self.bsas = []
        for a in archives:
            p = os.path.join(data_dir, a)
            if os.path.exists(p):
                self.bsas.append(BSA(p))

    def exists(self, name):
        name = name.lower().replace("/", "\\")
        return name in self.loose or any(name in b.files for b in self.bsas)

    def read(self, name):
        name = name.lower().replace("/", "\\")
        if name in self.loose:
            with open(self.loose[name], "rb") as f:
                return f.read()
        for b in reversed(self.bsas):
            if name in b.files:
                return b.read(name)
        return None

# --------------------------------------------------------------------------- ESM

MODEL_TYPES = {b"STAT", b"ACTI", b"DOOR", b"CONT", b"LIGH", b"MISC", b"WEAP", b"ARMO", b"CLOT",
               b"BOOK", b"ALCH", b"APPA", b"LOCK", b"PROB", b"REPA", b"INGR"}


def cstr(b):
    i = b.find(b"\0")
    return (b if i < 0 else b[:i]).decode("latin-1")


def subrecords(data):
    p = 0
    while p + 8 <= len(data):
        name = data[p:p + 4]
        size = struct.unpack_from("<I", data, p + 4)[0]
        yield name, data[p + 8:p + 8 + size]
        p += 8 + size


class ESM:
    def __init__(self):
        self.models = {}   # id (lower) -> mesh path
        self.cells = {}    # (gx, gy) -> list of refs (id, pos[3], rot[3], scale)
        self.lands = {}    # (gx, gy) -> dict
        self.ltex = {}     # index -> texture name

    def load(self, path):
        with open(path, "rb") as f:
            data = f.read()
        p = 0
        while p + 16 <= len(data):
            tag = data[p:p + 4]
            size = struct.unpack_from("<I", data, p + 4)[0]
            body = data[p + 16:p + 16 + size]
            p += 16 + size
            if tag in MODEL_TYPES:
                ident = model = None
                for n, d in subrecords(body):
                    if n == b"NAME": ident = cstr(d).lower()
                    elif n == b"MODL": model = cstr(d)
                if ident:
                    if model: self.models[ident] = model
                    else: self.models.pop(ident, None)
            elif tag == b"CELL":
                self._cell(body)
            elif tag == b"LAND":
                self._land(body)
            elif tag == b"LTEX":
                idx = name = None
                for n, d in subrecords(body):
                    if n == b"INTV": idx = struct.unpack("<I", d[:4])[0]
                    elif n == b"DATA": name = cstr(d)
                if idx is not None and name: self.ltex[idx] = name

    def _cell(self, body):
        grid = None
        refs = []
        cur = None
        for n, d in subrecords(body):
            if cur is None and n == b"DATA" and len(d) >= 12:
                flags, gx, gy = struct.unpack("<Iii", d[:12])
                if flags & 1:
                    return  # interior
                grid = (gx, gy)
            elif n == b"FRMR":
                cur = {"id": None, "scale": 1.0, "pos": None, "del": False}
                refs.append(cur)
            elif cur is not None:
                if n == b"NAME": cur["id"] = cstr(d).lower()
                elif n == b"XSCL": cur["scale"] = struct.unpack("<f", d[:4])[0]
                elif n == b"DELE": cur["del"] = True
                elif n == b"DATA" and len(d) >= 24: cur["pos"] = struct.unpack("<6f", d[:24])
        if grid is None:
            return
        lst = self.cells.setdefault(grid, [])
        lst.extend(r for r in refs if r["id"] and r["pos"] and not r["del"])

    def _land(self, body):
        land = {}
        for n, d in subrecords(body):
            if n == b"INTV": land["grid"] = struct.unpack("<ii", d[:8])
            elif n == b"VHGT" and len(d) >= 4 + 65 * 65:
                off = struct.unpack("<f", d[:4])[0]
                delta = np.frombuffer(d, np.int8, 65 * 65, 4).reshape(65, 65).astype(np.float64)
                rows = off + np.cumsum(delta[:, 0])
                h = np.cumsum(np.concatenate([rows[:, None], delta[:, 1:]], axis=1), axis=1)
                land["h"] = h * 8.0
            elif n == b"VNML" and len(d) >= 65 * 65 * 3:
                land["n"] = np.frombuffer(d, np.int8, 65 * 65 * 3).reshape(65, 65, 3).astype(np.float64)
            elif n == b"VCLR" and len(d) >= 65 * 65 * 3:
                land["c"] = np.frombuffer(d, np.uint8, 65 * 65 * 3).reshape(65, 65, 3).astype(np.float64) / 255
            elif n == b"VTEX" and len(d) >= 512:
                raw = np.frombuffer(d, np.uint16, 256)
                out = np.zeros(256, np.uint16)
                i = 0
                for y1 in range(4):
                    for x1 in range(4):
                        for y2 in range(4):
                            for x2 in range(4):
                                out[(y1 * 4 + y2) * 16 + x1 * 4 + x2] = raw[i]; i += 1
                land["t"] = out.reshape(16, 16)
        if "grid" in land:
            self.lands[land["grid"]] = land

# --------------------------------------------------------------------------- NIF

class NifError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.d = data; self.p = 0
    def u8(self): v = self.d[self.p]; self.p += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v
    def i32(self): v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v
    def u32(self): v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v
    def f(self): v = struct.unpack_from("<f", self.d, self.p)[0]; self.p += 4; return v
    def fs(self, n):
        v = np.frombuffer(self.d, np.float32, n, self.p).astype(np.float64); self.p += 4 * n; return v
    def u16s(self, n):
        v = np.frombuffer(self.d, np.uint16, n, self.p); self.p += 2 * n; return v
    def skip(self, n): self.p += n
    def string(self):
        n = self.u32()
        if n > 100000: raise NifError("bad string")
        s = self.d[self.p:self.p + n].decode("latin-1"); self.p += n; return s
    def reflist(self):
        n = self.u32()
        if n > 100000: raise NifError("bad list")
        return [self.i32() for _ in range(n)]


def nif_keys(r, kind, force=False):
    """kind: number of floats per value; 'q' = quaternion"""
    count = r.u32()
    if count == 0 and not force:
        return 0
    t = r.u32()
    vals = 4 if kind == "q" else kind
    if t == 1: r.skip(count * 4 * (1 + vals))
    elif t == 2: r.skip(count * 4 * (1 + (vals if kind == "q" else vals * 3)))
    elif t == 3: r.skip(count * 4 * (1 + vals + 3))
    elif t == 4: pass
    elif t == 0 and count == 0: pass
    else: raise NifError("key type %d" % t)
    return t


def parse_nif(data):
    r = Reader(data)
    nl = data.index(b"\n")
    if not data.startswith(b"NetImmerse File Format"):
        raise NifError("header")
    r.p = nl + 1
    ver = r.u32()
    if ver not in (0x04000002, 0x04000000):
        raise NifError("version %x" % ver)
    n = r.i32()
    blocks = []

    def named(b):
        b["name"] = r.string(); b["extra"] = r.i32(); b["ctrl"] = r.i32()

    def avobj(b):
        named(b)
        b["flags"] = r.u16()
        b["t"] = r.fs(3); b["r"] = r.fs(9).reshape(3, 3); b["s"] = r.f()
        r.skip(12)
        b["props"] = r.reflist()
        if r.i32():
            r.skip(4 + 15 * 4)

    def node(b):
        avobj(b); b["children"] = r.reflist(); r.reflist()

    def prop(b):
        named(b); b["pflags"] = r.u16()

    def controller(b):
        r.i32(); r.u16(); r.skip(16); r.i32()

    def controlled(b):
        r.i32(); r.i32()

    def shapedata(b):
        nv = r.u16()
        b["v"] = r.fs(nv * 3).reshape(nv, 3) if r.i32() else None
        b["n"] = r.fs(nv * 3).reshape(nv, 3) if r.i32() else None
        r.skip(16)
        b["c"] = r.fs(nv * 4).reshape(nv, 4) if r.i32() else None
        uvs = r.u16() & 0x3f
        b["uv"] = r.fs(uvs * nv * 2).reshape(uvs, nv, 2)[0] if (r.i32() and uvs) else None
        return nv

    for i in range(n):
        typ = r.string()
        b = {"type": typ}
        blocks.append(b)
        if typ in ("NiNode", "AvoidNode", "NiBSParticleNode", "NiBSAnimationNode", "NiBillboardNode",
                   "RootCollisionNode"):
            node(b)
        elif typ == "NiSwitchNode":
            node(b); r.i32()
        elif typ == "NiLODNode":
            node(b); r.i32(); r.skip(12); k = r.u32(); r.skip(8 * k)
        elif typ == "NiTriShape":
            avobj(b); b["data"] = r.i32(); b["skin"] = r.i32()
        elif typ in ("NiRotatingParticles", "NiAutoNormalParticles"):
            avobj(b); r.i32(); r.i32()
        elif typ == "NiCamera":
            avobj(b); r.skip(11 * 4 + 8)
        elif typ == "NiTexturingProperty":
            prop(b); r.i32(); r.i32()
            texs = []
            for k in range(7):
                if r.i32():
                    src = r.i32(); clamp = r.i32(); r.i32(); r.i32(); r.skip(6)
                    texs.append((src, clamp))
                else:
                    texs.append(None)
                if k == 5 and texs[5] is not None:
                    r.skip(24)
            b["base"] = texs[0]
        elif typ == "NiFogProperty":
            prop(b); r.skip(16)
        elif typ == "NiMaterialProperty":
            prop(b); m = r.fs(12); b["amb"] = m[0:3]; b["dif"] = m[3:6]; b["emi"] = m[9:12]
            r.f(); b["alpha"] = r.f()
        elif typ in ("NiZBufferProperty", "NiShadeProperty", "NiDitherProperty", "NiWireframeProperty",
                     "NiSpecularProperty"):
            prop(b)
        elif typ == "NiAlphaProperty":
            prop(b); b["thr"] = r.u8()
        elif typ == "NiVertexColorProperty":
            prop(b); b["vmode"] = r.i32(); b["lmode"] = r.i32()
        elif typ == "NiStencilProperty":
            prop(b); r.u8(); r.skip(24); b["draw"] = r.i32()
        elif typ in ("NiVisController", "NiKeyframeController", "NiAlphaController", "NiRollController",
                     "NiMaterialColorController", "NiLookAtController"):
            controller(b); r.i32()
        elif typ == "NiGeomMorpherController":
            controller(b); r.i32(); r.u8()
        elif typ == "NiUVController":
            controller(b); r.u16(); r.i32()
        elif typ == "NiPathController":
            controller(b); r.skip(14); r.i32(); r.i32()
        elif typ == "NiFlipController":
            controller(b); r.u32(); r.u32(); r.f(); r.reflist()
        elif typ in ("NiParticleSystemController", "NiBSPArrayController"):
            controller(b)
            r.skip(6 * 4 + 12 + 16 + 12); r.u8(); r.skip(12); r.u16(); r.skip(12); r.i32(); r.skip(16)
            np_ = r.u16(); r.u16(); r.skip(np_ * (12 + 12 + 12 + 4))
            r.u32(); r.i32(); r.i32(); r.u8()
        elif typ in ("NiAmbientLight", "NiDirectionalLight", "NiPointLight", "NiSpotLight"):
            avobj(b); k = r.u32(); r.skip(4 * k); r.skip(4 + 36)
            if typ in ("NiPointLight", "NiSpotLight"): r.skip(12)
            if typ == "NiSpotLight": r.skip(8)
        elif typ == "NiTextureEffect":
            avobj(b); k = r.u32(); r.skip(4 * k); r.skip(36 + 12 + 4 + 12); r.i32(); r.skip(23)
        elif typ == "NiVertWeightsExtraData":
            r.i32(); r.i32(); k = r.u16(); r.skip(4 * k)
        elif typ == "NiTextKeyExtraData":
            r.i32(); r.i32(); k = r.i32()
            for _ in range(k): r.f(); r.string()
        elif typ == "NiStringExtraData":
            b["next"] = r.i32(); r.i32(); b["str"] = r.string()
        elif typ == "NiGravity":
            controlled(b); r.skip(4 + 4 + 4 + 12 + 12)
        elif typ == "NiPlanarCollider":
            controlled(b); r.skip(4 * 12 + 12 + 4)
        elif typ == "NiSphericalCollider":
            controlled(b); r.skip(8 + 12)
        elif typ == "NiParticleGrowFade":
            controlled(b); r.skip(8)
        elif typ == "NiParticleColorModifier":
            controlled(b); r.i32()
        elif typ == "NiParticleRotation":
            controlled(b); r.skip(17)
        elif typ == "NiFloatData":
            nif_keys(r, 1)
        elif typ == "NiPosData":
            nif_keys(r, 3)
        elif typ == "NiColorData":
            nif_keys(r, 4)
        elif typ == "NiUVData":
            for _ in range(4): nif_keys(r, 1)
        elif typ == "NiVisData":
            k = r.i32(); r.skip(5 * k)
        elif typ == "NiKeyframeData":
            if nif_keys(r, "q") == 4:
                r.f(); nif_keys(r, 1, True); nif_keys(r, 1, True); nif_keys(r, 1, True)
            nif_keys(r, 3); nif_keys(r, 1)
        elif typ == "NiMorphData":
            mc = r.i32(); vc = r.i32(); r.u8()
            for _ in range(mc):
                nif_keys(r, 1, True); r.skip(vc * 12)
        elif typ == "NiPixelData":
            r.skip(4 * 6 + 12); nm = r.i32(); r.i32(); r.skip(nm * 12); k = r.i32(); r.skip(k)
        elif typ == "NiTriShapeData":
            shapedata(b)
            r.u16(); cnt = r.i32(); b["tri"] = r.u16s(cnt).reshape(-1, 3)
            k = r.u16()
            for _ in range(k):
                m = r.u16(); r.skip(2 * m)
        elif typ in ("NiAutoNormalParticlesData", "NiRotatingParticlesData"):
            shapedata(b)
            nv = 0 if b["v"] is None else len(b["v"])
            r.u16(); r.f(); r.u16()
            if r.i32(): r.skip(4 * nv)
            if typ == "NiRotatingParticlesData" and r.i32(): r.skip(16 * nv)
        elif typ == "NiSkinData":
            r.skip(52); k = r.i32(); r.i32()
            for _ in range(k):
                r.skip(4 * 17); w = r.u16(); r.skip(6 * w)
        elif typ == "NiSkinInstance":
            r.i32(); r.i32(); r.reflist()
        elif typ == "NiSourceTexture":
            named(b)
            if r.u8():
                b["file"] = r.string()
            else:
                r.u8(); r.i32(); b["file"] = None
            r.skip(12); r.u8()
        elif typ == "NiSequenceStreamHelper":
            named(b)
        else:
            raise NifError("unknown block " + typ)
    return blocks


def nif_shapes(blocks):
    """Yields (vertices, normals, colors, uv, triangles, material) in model space."""
    if not blocks:
        return
    marker = False
    root = blocks[0]
    e = root.get("extra", -1)
    seen = 0
    while 0 <= e < len(blocks) and seen < 32:
        x = blocks[e]
        if x["type"] == "NiStringExtraData" and x.get("str", "").startswith("MRK"):
            marker = True
        e = x.get("next", -1); seen += 1

    out = []

    def walk(i, M, props, depth):
        if i < 0 or i >= len(blocks) or depth > 64:
            return
        b = blocks[i]
        t = b["type"]
        if t in ("RootCollisionNode", "NiBSParticleNode", "NiCamera"):
            return
        if "flags" not in b or (b["flags"] & 1):
            return
        R = b["r"] * b["s"]
        if depth == 0 and b["name"].lower() != "bip01":
            L = np.eye(4)
        else:
            L = np.eye(4); L[:3, :3] = R; L[:3, 3] = b["t"]
        W = M @ L
        pr = dict(props)
        for p in b.get("props", []):
            if 0 <= p < len(blocks):
                pr[blocks[p]["type"]] = blocks[p]
        if t == "NiTriShape":
            if b["skin"] >= 0:
                return
            if marker and b["name"].lower().startswith("tri editormarker"):
                return
            d = blocks[b["data"]] if 0 <= b["data"] < len(blocks) else None
            if not d or d["type"] != "NiTriShapeData" or d["v"] is None or len(d["tri"]) == 0:
                return
            v = d["v"] @ W[:3, :3].T + W[:3, 3]
            nrm = None
            if d["n"] is not None:
                nrm = d["n"] @ W[:3, :3].T
                l = np.linalg.norm(nrm, axis=1, keepdims=True); l[l == 0] = 1
                nrm = nrm / l
            out.append((v, nrm, d["c"], d["uv"], d["tri"], pr))
            return
        if t in ("NiSwitchNode", "NiLODNode"):
            kids = b["children"][:1]
        else:
            kids = b.get("children", [])
        for c in kids:
            walk(c, W, pr, depth + 1)

    walk(0, np.eye(4), {}, 0)
    for s in out:
        yield s


def material_of(pr, blocks):
    tex = None
    tp = pr.get("NiTexturingProperty")
    if tp and tp.get("base"):
        src = tp["base"][0]
        if 0 <= src < len(blocks) and blocks[src]["type"] == "NiSourceTexture":
            tex = blocks[src].get("file")
    alpha = 0
    ap = pr.get("NiAlphaProperty")
    if ap:
        fl = ap["pflags"]
        if fl & 0x200 or fl & 1:
            alpha = 1
    double = 1 if (pr.get("NiStencilProperty", {}).get("draw", 1) == 3) else 0
    mat = pr.get("NiMaterialProperty")
    dif = mat["dif"] if mat else np.ones(3)
    emi = mat["emi"] if mat else np.zeros(3)
    vmode = pr.get("NiVertexColorProperty", {}).get("vmode", 2)
    return tex, alpha, double, dif, emi, vmode

# --------------------------------------------------------------------------- textures

def morton_index(w, h):
    y, x = np.mgrid[0:h, 0:w]
    tile = (y >> 3) * (w >> 3) + (x >> 3)
    xx, yy = x & 7, y & 7
    m = (xx & 1) | ((yy & 1) << 1) | ((xx & 2) << 1) | ((yy & 2) << 2) | ((xx & 4) << 2) | ((yy & 4) << 3)
    return (tile * 64 + m).ravel()


_morton = {}


def to_3ds(img, fmt):
    """img: PIL RGBA, power of two >= 8. Returns tiled bytes of all mip levels and level count."""
    out = []
    levels = 0
    cur = img
    while True:
        w, h = cur.size
        a = np.asarray(cur, np.uint32)[::-1]   # GPU rows run bottom-up
        if fmt == 0:
            px = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
        else:
            px = ((a[..., 0] >> 4) << 12) | ((a[..., 1] >> 4) << 8) | ((a[..., 2] >> 4) << 4) | (a[..., 3] >> 4)
        key = (w, h)
        if key not in _morton: _morton[key] = morton_index(w, h)
        dst = np.zeros(w * h, np.uint16)
        dst[_morton[key]] = px.ravel().astype(np.uint16)
        out.append(dst.tobytes())
        levels += 1
        if w <= 8 or h <= 8:
            break
        cur = cur.resize((w // 2, h // 2), Image.BOX)
    return b"".join(out), levels


def pow2_floor(n):
    p = 8
    while p * 2 <= n: p *= 2
    return p


class TexStore:
    def __init__(self, vfs, maxsize):
        self.vfs = vfs; self.max = maxsize
        self.list = []        # (w, h, fmt, levels, bytes)
        self.index = {}       # (name, alpha) -> idx
        self.cache = {}       # name -> PIL image or None
        self.missing = set()
        white = Image.new("RGBA", (8, 8), (255, 255, 255, 255))
        wd, wl = to_3ds(white, 0)
        self.list.append((8, 8, 0, wl, wd))

    def image(self, name):
        name = name.lower().replace("/", "\\")
        if name in self.cache:
            return self.cache[name]
        base = os.path.splitext(name)[0]
        if not base.startswith("textures\\"):
            base = "textures\\" + base
        img = None
        for ext in (".dds", ".tga", ".bmp"):
            d = self.vfs.read(base + ext)
            if d:
                try:
                    img = Image.open(io.BytesIO(d)); img.load(); img = img.convert("RGBA")
                except Exception as ex:
                    print("  texture %s: %s" % (base + ext, ex)); img = None
                break
        if img is None:
            self.missing.add(name)
        self.cache[name] = img
        return img

    def get(self, name, alpha):
        if not name:
            return 0
        key = (name.lower(), alpha)
        if key in self.index:
            return self.index[key]
        img = self.image(name)
        if img is None:
            self.index[key] = 0
            return 0
        w = min(pow2_floor(img.size[0]), self.max); h = min(pow2_floor(img.size[1]), self.max)
        im = img.resize((w, h), Image.LANCZOS)
        fmt = 1 if alpha else 0
        data, levels = to_3ds(im, fmt)
        self.list.append((w, h, fmt, levels, data))
        self.index[key] = len(self.list) - 1
        return self.index[key]

    def write(self, path):
        with open(path, "wb") as f:
            f.write(b"MWT1" + struct.pack("<I", len(self.list)))
            for w, h, fmt, levels, data in self.list:
                f.write(struct.pack("<HHBBHI", w, h, fmt, levels, 0, len(data)))
                f.write(data)

# --------------------------------------------------------------------------- baking

def light(normals, base, emi=None):
    if normals is None:
        k = AMBIENT + SUNCOL * 0.6
        c = base * k
    else:
        d = np.clip(normals @ SUN, 0, 1)[:, None]
        c = base * (AMBIENT + SUNCOL * d)
    if emi is not None:
        c = c + emi
    return np.clip(c, 0, 1)


class Batch:
    def __init__(self, tex, flags, opacity=255):
        self.tex = tex; self.flags = flags; self.opacity = opacity
        self.parts = []   # (pos float local, uv, rgba uint8, tri)
        self.nv = 0

    def add(self, pos, uv, col, tri):
        self.parts.append((pos, uv, col, tri)); self.nv += len(pos)


class ModelCache:
    def __init__(self, vfs, tex):
        self.vfs = vfs; self.tex = tex; self.cache = {}; self.failed = {}

    def get(self, path):
        key = path.lower()
        if key in self.cache:
            return self.cache[key]
        data = self.vfs.read("meshes\\" + key)
        shapes = []
        if data is None:
            self.failed[key] = "missing"
        else:
            try:
                blocks = parse_nif(data)
                for v, n, c, uv, tri, pr in nif_shapes(blocks):
                    tname, alpha, double, dif, emi, vmode = material_of(pr, blocks)
                    if c is not None and vmode != 0:
                        base = c[:, :3]
                        a = c[:, 3]
                    else:
                        base = np.tile(dif, (len(v), 1)); a = np.ones(len(v))
                    rgb = light(n, base, emi)
                    if uv is None: uv = np.zeros((len(v), 2))
                    uv = np.stack([uv[:, 0], 1.0 - uv[:, 1]], axis=1)
                    uv = uv - np.floor(uv.mean(axis=0))   # whole repeats off: stays in the int16 range
                    shapes.append({"v": v, "n": n, "base": base, "a": a, "emi": emi, "uv": uv,
                                   "tri": tri.astype(np.int64), "tex": tname, "alpha": alpha, "double": double})
            except Exception as ex:
                self.failed[key] = str(ex)
        self.cache[key] = shapes
        return shapes


def rot_matrix(rx, ry, rz):
    def X(a):
        c, s = math.cos(a), math.sin(a); return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    def Y(a):
        c, s = math.cos(a), math.sin(a); return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    def Z(a):
        c, s = math.cos(a), math.sin(a); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
    return X(-rx) @ Y(-ry) @ Z(-rz)


def terrain_texture(esm, tex, gx, gy, size):
    """Blends the land textures of a cell into one image (size x size), like a composite map."""
    def vtex(cx, cy, x, y):
        while x < 0: x += 16; cx -= 1
        while x >= 16: x -= 16; cx += 1
        while y < 0: y += 16; cy -= 1
        while y >= 16: y -= 16; cy += 1
        l = esm.lands.get((cx, cy))
        if l is None or "t" not in l: return 0
        return int(l["t"][y, x])

    # blend points every 512 units (17 x 17), point (i, j) carries the texture of tile (i - 1, j)
    ids = np.zeros((17, 17), np.int64)
    for j in range(17):
        for i in range(17):
            ids[j, i] = vtex(gx, gy, i - 1, j)
    px = size // 16   # pixels per texture repeat
    layers = {}
    for t in np.unique(ids):
        name = "_land_default.dds" if t == 0 else esm.ltex.get(int(t) - 1, "_land_default.dds")
        img = tex.image(name)
        if img is None:
            arr = np.full((px, px, 3), 128.0)
        else:
            arr = np.asarray(img.convert("RGB").resize((px, px), Image.LANCZOS), np.float64)
        layers[int(t)] = np.tile(arr, (16, 16, 1))   # size x size, row 0 = south here
    # bilinear weights of the blend points per pixel
    coord = (np.arange(size) + 0.5) / size * 16
    i0 = np.clip(np.floor(coord).astype(int), 0, 15); f = coord - i0
    out = np.zeros((size, size, 3))
    for t, layer in layers.items():
        m = (ids == t).astype(np.float64)
        w = (m[i0][:, i0] * (1 - f)[:, None] * (1 - f)[None, :] +
             m[i0][:, i0 + 1] * (1 - f)[:, None] * f[None, :] +
             m[i0 + 1][:, i0] * f[:, None] * (1 - f)[None, :] +
             m[i0 + 1][:, i0 + 1] * f[:, None] * f[None, :])
        out += layer * w[..., None]
    # rows: south first -> image rows north first
    img = Image.fromarray(np.clip(out[::-1], 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    return to_3ds(img, 0)


def bake_cell(esm, models, tex, gx, gy, stats):
    ox, oy = gx * CELL + HALF, gy * CELL + HALF
    batches = []    # list of Batch
    land = esm.lands.get((gx, gy))
    terrain_tex = None
    minh = 0.0
    if land is not None and "h" in land:
        h = land["h"]
        minh = h.min()
        n = land.get("n")
        if n is not None:
            n = n.reshape(-1, 3); n = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)
        c = land["c"].reshape(-1, 3) if "c" in land else np.ones((65 * 65, 3))
        rgb = light(n, c)
        terrain_tex = terrain_texture(esm, tex, gx, gy, stats["terrain_tex"])
        ys, xs = np.mgrid[0:65, 0:65]
        pos = np.stack([xs * 128.0 - HALF, ys * 128.0 - HALF, h], axis=-1).reshape(-1, 3)
        uv = np.stack([xs / 64.0, ys / 64.0], axis=-1).reshape(-1, 2)
        col = np.concatenate([rgb * 255, np.full((65 * 65, 1), 255)], axis=1).astype(np.uint8)
        for qy in range(2):
            for qx in range(2):
                sel = np.zeros((65, 65), bool)
                sel[qy * 32:qy * 32 + 33, qx * 32:qx * 32 + 33] = True
                idx = np.nonzero(sel.ravel())[0]
                remap = -np.ones(65 * 65, np.int64); remap[idx] = np.arange(len(idx))
                tris = []
                for y in range(qy * 32, qy * 32 + 32):
                    for x in range(qx * 32, qx * 32 + 32):
                        a = y * 65 + x; b = a + 1; c_ = a + 65; d = c_ + 1
                        tris.append((a, b, d)); tris.append((a, d, c_))
                tris = remap[np.array(tris)]
                bt = Batch(0xFFFF, 0)
                bt.add(pos[idx], uv[idx], col[idx], tris)
                batches.append(bt)
                stats["terrain_tris"] += len(tris)
    # objects, merged per sub-chunk (2 x 2) and material
    groups = {}
    nrefs = 0
    for ref in esm.cells.get((gx, gy), []):
        mpath = esm.models.get(ref["id"])
        if not mpath:
            continue
        if "marker" in ref["id"]:
            continue
        shapes = models.get(mpath)
        if not shapes:
            continue
        nrefs += 1
        px, py, pz, rx, ry, rz = ref["pos"]
        R = rot_matrix(rx, ry, rz) * ref["scale"]
        Rn = rot_matrix(rx, ry, rz)
        lx, ly = px - ox, py - oy
        sub = (0 if lx < 0 else 1) + (0 if ly < 0 else 2)
        # size class (docs/POMYSLY.md, step 1): small things (grass, kelp, shells: radius < 180) get their own
        # batches, drawn only near; things wholly under the water too (seen from the land only close by)
        allv = np.concatenate([s["v"] for s in shapes])
        rad = float(np.linalg.norm(allv, axis=1).max()) * ref["scale"]
        top = pz + float((allv @ R.T)[:, 2].max())
        cls = (8 if rad < SMALL_RADIUS else 0) | (16 if top < 0 else 0)
        stats["small_tris" if cls & 8 else "under_tris" if cls & 16 else "big_tris"] += sum(len(s["tri"]) for s in shapes)
        for s in shapes:
            v = s["v"] @ R.T + np.array([lx, ly, pz])
            nrm = None if s["n"] is None else s["n"] @ Rn.T
            rgb = light(nrm, s["base"], s["emi"])
            col = np.concatenate([rgb * 255, s["a"][:, None] * 255], axis=1)
            col = np.clip(col, 0, 255).astype(np.uint8)
            ti = tex.get(s["tex"], s["alpha"])
            flags = (1 if s["alpha"] else 0) | (2 if s["double"] else 0) | cls
            key = (sub, ti, flags)
            bl = groups.get(key)
            if bl is None or bl[-1].nv + len(v) > 65000:
                bl = groups.setdefault(key, [])
                bl.append(Batch(ti, flags))
            bl[-1].add(v, s["uv"], col, s["tri"])
            stats["object_tris"] += len(s["tri"])
    for key in sorted(groups):
        batches.extend(groups[key])
    if land is None or minh < 0:
        # water at z = 0
        bt = Batch(0, 4, 150)
        p = np.array([[-HALF, -HALF, 0], [HALF, -HALF, 0], [HALF, HALF, 0], [-HALF, HALF, 0]])
        col = np.array([[50, 85, 105, 255]] * 4, np.uint8)
        bt.add(p, np.zeros((4, 2)), col, np.array([[0, 1, 2], [0, 2, 3]]))
        batches.append(bt)
    stats["refs"] += nrefs
    return batches, terrain_tex


def ground(esm, x, y):
    """Terrain height at a world position (bilinear over the 65 x 65 heights), 0 without land."""
    gx, gy = math.floor(x / CELL), math.floor(y / CELL)
    land = esm.lands.get((gx, gy))
    if land is None or "h" not in land:
        return 0.0
    fx = (x - gx * CELL) / 128.0; fy = (y - gy * CELL) / 128.0
    ix = min(int(fx), 63); iy = min(int(fy), 63); u = fx - ix; v = fy - iy
    h = land["h"]
    return float(h[iy, ix] * (1 - u) * (1 - v) + h[iy, ix + 1] * u * (1 - v) +
                 h[iy + 1, ix] * (1 - u) * v + h[iy + 1, ix + 1] * u * v)


def align(f, n=16):
    pad = (-f.tell()) % n
    if pad: f.write(b"\0" * pad)


def write_cell(path, gx, gy, batches, terrain_tex, tsize):
    with open(path, "wb") as f:
        hdr = struct.Struct("<4siiHHIII")
        f.write(b"\0" * hdr.size)
        align(f)
        ttofs = 0
        if terrain_tex is not None:
            ttofs = f.tell(); f.write(terrain_tex[0]); align(f)
        else:
            tsize = 0
        bofs = f.tell()
        bstruct = struct.Struct("<HBBIIII4f")
        f.write(b"\0" * (bstruct.size * len(batches)))
        align(f)
        recs = []
        for b in batches:
            pos = np.concatenate([p[0] for p in b.parts])
            uv = np.concatenate([p[1] for p in b.parts])
            col = np.concatenate([p[2] for p in b.parts])
            tris = []
            base = 0
            for p in b.parts:
                tris.append(p[3] + base); base += len(p[0])
            tris = np.concatenate(tris).astype(np.uint16)
            uvs = np.clip(np.round(uv * UV_SCALE), -32767, 32767).astype(np.int16)
            ps = np.clip(np.round(pos), -32767, 32767).astype(np.int16)
            vtx = np.zeros(len(pos), dtype=[("p", "<i2", 4), ("t", "<i2", 2), ("c", "u1", 4)])
            vtx["p"][:, :3] = ps; vtx["t"] = uvs; vtx["c"] = col
            vofs = f.tell(); f.write(vtx.tobytes()); align(f)
            iofs = f.tell(); f.write(tris.tobytes()); align(f)
            lo, hi = pos.min(axis=0), pos.max(axis=0)
            c = (lo + hi) / 2; rad = float(np.linalg.norm(hi - lo) / 2)
            recs.append(bstruct.pack(b.tex, b.flags, b.opacity, vofs, len(pos), iofs, len(tris.ravel()),
                                     c[0], c[1], c[2], rad))
        end = f.tell()
        f.seek(bofs); f.write(b"".join(recs))
        f.seek(0); f.write(hdr.pack(b"MWC1", gx, gy, len(batches), tsize, ttofs, bofs, end))


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--data", default=r"D:\Programowanie\OpenMorrowind\data\Data Files")
    ap.add_argument("--center", type=int, nargs=2, default=[-2, -9], help="start cell (Seyda Neen: -2 -9)")
    ap.add_argument("--rect", type=int, nargs=4, default=[-4, -11, 2, -5],
                    help="cells to bake: x0 y0 x1 y1 (default: Seyda Neen to past Pelagiad)")
    ap.add_argument("--radius", type=int, default=None, help="instead of --rect: a square round --center")
    ap.add_argument("--route-to", type=int, nargs=2, default=[0, -7],
                    help="the benchmark walks from --center's cell to this one (Pelagiad: 0 -7)")
    ap.add_argument("--tex", type=int, default=128, help="max object texture size")
    ap.add_argument("--terrain-tex", type=int, default=256, help="composite terrain texture per cell")
    ap.add_argument("--out", default=os.path.join(here, "..", "romfs"))
    a = ap.parse_args()

    t0 = time.time()
    vfs = VFS(a.data, ["Morrowind.bsa"])
    esm = ESM(); esm.load(os.path.join(a.data, "Morrowind.esm"))
    print("esm: %d models, %d exterior cells, %d lands, %.1fs" % (len(esm.models), len(esm.cells), len(esm.lands), time.time() - t0))
    tex = TexStore(vfs, a.tex)
    models = ModelCache(vfs, tex)
    os.makedirs(os.path.join(a.out, "cells"), exist_ok=True)
    for fn in os.listdir(os.path.join(a.out, "cells")):
        os.remove(os.path.join(a.out, "cells", fn))
    done = []
    stats = {"terrain_tris": 0, "object_tris": 0, "refs": 0, "terrain_tex": a.terrain_tex, "small_tris": 0, "under_tris": 0, "big_tris": 0}
    cx, cy = a.center
    total_bytes = 0
    if a.radius is not None:
        x0, y0, x1, y1 = cx - a.radius, cy - a.radius, cx + a.radius, cy + a.radius
    else:
        x0, y0, x1, y1 = a.rect
    sizes = {}
    for gy in range(y0, y1 + 1):
        for gx in range(x0, x1 + 1):
            if (gx, gy) not in esm.lands and (gx, gy) not in esm.cells:
                continue
            before = dict(stats)
            batches, ttex = bake_cell(esm, models, tex, gx, gy, stats)
            p = os.path.join(a.out, "cells", "%d_%d.bin" % (gx, gy))
            write_cell(p, gx, gy, batches, ttex, a.terrain_tex)
            sz = os.path.getsize(p); total_bytes += sz; sizes[(gx, gy)] = sz
            done.append((gx, gy))
            print("cell %3d,%3d: %4d refs, %6d object tris, %5d terrain tris, %3d batches, %6.0f KB" % (
                gx, gy, stats["refs"] - before["refs"], stats["object_tris"] - before["object_tris"],
                stats["terrain_tris"] - before["terrain_tris"], len(batches), sz / 1024))
    tex.write(os.path.join(a.out, "textures.bin"))
    # start: the centre cell, above the ground
    land = esm.lands.get((cx, cy))
    gz = float(land["h"][32, 32]) if land is not None and "h" in land else 0.0
    sx, sy = cx * CELL + HALF, cy * CELL + HALF
    # the benchmark's route: a straight line from the start cell's centre to --route-to's centre, a point every
    # 256 units at eye height over the ground (or the water)
    tx, ty = a.route_to[0] * CELL + HALF, a.route_to[1] * CELL + HALF
    steps = max(2, int(math.hypot(tx - sx, ty - sy) / 256))
    route = []
    for i in range(steps + 1):
        x = sx + (tx - sx) * i / steps; y = sy + (ty - sy) * i / steps
        route.append((x, y, max(ground(esm, x, y), 0.0) + 160.0))
    with open(os.path.join(a.out, "index.bin"), "wb") as f:
        f.write(b"MWI2" + struct.pack("<I", len(done)))
        for gx, gy in done: f.write(struct.pack("<iiI", gx, gy, sizes[(gx, gy)]))
        f.write(struct.pack("<4f", sx, sy, max(gz, 0) + 400.0, 0.6))
        f.write(struct.pack("<I", len(route)))
        for p in route: f.write(struct.pack("<3f", *p))
    print("route: %d points, %.0f units" % (len(route), math.hypot(tx - sx, ty - sy)))
    tbytes = os.path.getsize(os.path.join(a.out, "textures.bin"))
    print("%d cells, %d refs, %d object tris, %d terrain tris" % (len(done), stats["refs"], stats["object_tris"], stats["terrain_tris"]))
    print("object tris by class: big %d, small %d, under water %d" % (stats["big_tris"], stats["small_tris"], stats["under_tris"]))
    print("textures: %d (%.0f KB), cells %.0f KB, models parsed %d, failed %d, missing textures %d, %.1fs" % (
        len(tex.list), tbytes / 1024, total_bytes / 1024, len(models.cache), len(models.failed), len(tex.missing), time.time() - t0))
    for k, v in list(models.failed.items())[:20]:
        print("  model %s: %s" % (k, v))
    for k in list(tex.missing)[:10]:
        print("  missing texture %s" % k)


if __name__ == "__main__":
    main()
