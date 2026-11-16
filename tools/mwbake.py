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
import argparse, math, os, struct, sys, time, io, subprocess, tempfile
from concurrent.futures import ThreadPoolExecutor
from simplify import lod_shape
import numpy as np
from PIL import Image, ImageEnhance

# The 3DS screen is paler than a PC monitor: the textures get more colour and contrast here
# (free at run time). --saturation / --contrast, 1 = as they are.
SATURATION, CONTRAST = 1.0, 1.0

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
# Dynamic objects (docs/PLAN.md step 1): not merged into the cell's statics but written to objects.bin with their
# FRMR id, so the game can take, open and move them. Type numbers as in objects.bin (1 item, 2 door, 3 container).
ITEM_TYPES = {b"MISC", b"WEAP", b"ARMO", b"CLOT", b"BOOK", b"ALCH", b"APPA", b"LOCK", b"PROB", b"REPA", b"INGR"}
OT_ITEM, OT_DOOR, OT_CONT = 1, 2, 3
def obj_type(tag):
    return OT_ITEM if tag in ITEM_TYPES else OT_DOOR if tag == b"DOOR" else OT_CONT if tag == b"CONT" else 0


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
        self.kind = {}     # id (lower) -> record tag (b"MISC", b"DOOR"...) of the things with a model
        self.names = {}    # id (lower) -> the name shown in the game (FNAM)
        self.interiors = {}    # cell name -> {"refs": [...], "ambi": (ambient, sunlight, fog) rgb 0..1}
        self.lights = {}       # LIGH id -> (radius, rgb 0..1)
        self.organic = set()   # containers that are plants to pick (flora_, kollop_, trama roots): stay static for now

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
                ident = model = fname = None
                organic = False
                for n, d in subrecords(body):
                    if n == b"NAME": ident = cstr(d).lower()
                    elif n == b"MODL": model = cstr(d)
                    elif n == b"LHDT" and tag == b"LIGH" and len(d) >= 24:
                        rad = struct.unpack_from("<i", d, 12)[0]
                        fl = struct.unpack_from("<I", d, 20)[0]
                        if not fl & 0x100:   # (negative lights: none)
                            self.lights[ident or ""] = (float(rad), np.frombuffer(d[16:19], np.uint8) / 255.0)
                    elif n == b"FLAG" and tag == b"CONT" and len(d) >= 4 and struct.unpack("<I", d[:4])[0] & 1:
                        organic = True
                    elif n == b"FNAM": fname = d.split(b"\0")[0].decode("cp1250", "replace")
                if ident:
                    self.kind[ident] = tag
                    if organic: self.organic.add(ident)
                    else: self.organic.discard(ident)
                    if fname: self.names[ident] = fname
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
        cname, inter, ambi = None, False, None
        for n, d in subrecords(body):
            if cur is None and n == b"NAME": cname = cstr(d)
            elif cur is None and n == b"AMBI" and len(d) >= 12:
                ambi = tuple(np.frombuffer(d[k * 4:k * 4 + 3], np.uint8) / 255.0 for k in range(3))
            elif cur is None and n == b"DATA" and len(d) >= 12:
                flags, gx, gy = struct.unpack("<Iii", d[:12])
                if flags & 1:
                    inter = True   # an interior: kept by its name (baked only when asked: --interiors)
                else:
                    grid = (gx, gy)
            elif n == b"FRMR":
                cur = {"id": None, "scale": 1.0, "pos": None, "del": False, "frmr": struct.unpack("<I", d[:4])[0]}
                refs.append(cur)
            elif cur is not None:
                if n == b"NAME": cur["id"] = cstr(d).lower()
                elif n == b"XSCL": cur["scale"] = struct.unpack("<f", d[:4])[0]
                elif n == b"DELE": cur["del"] = True
                elif n == b"DATA" and len(d) >= 24: cur["pos"] = struct.unpack("<6f", d[:24])
                elif n == b"DODT":   # a load door: it takes you somewhere (where to stand, facing)
                    cur.setdefault("dest", ""); cur["destpos"] = struct.unpack("<6f", d[:24])
                elif n == b"DNAM": cur["dest"] = cstr(d)          # ... into this interior (no DNAM: outside)
        if inter and cname:
            e = self.interiors.setdefault(cname, {"refs": [], "ambi": None})
            e["refs"].extend(r for r in refs if r["id"] and r["pos"] and not r["del"])
            if ambi: e["ambi"] = ambi
            return
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


def read_keys(r, kind, force=False):
    """Keys: kind = floats per value, 'q' = quaternion. Returns (type, times, values) - the
    values only (tangents and TBC dropped: the character baker interpolates linearly)."""
    count = r.u32()
    if count == 0 and not force:
        return 0, None, None
    t = r.u32()
    vals = 4 if kind == "q" else kind
    if t == 1: per = 1 + vals
    elif t == 2: per = 1 + (vals if kind == "q" else vals * 3)
    elif t == 3: per = 1 + vals + 3
    elif t == 4 or (t == 0 and count == 0): return t, None, None
    else: raise NifError("key type %d" % t)
    a = r.fs(count * per).reshape(count, per) if count else np.zeros((0, per))
    return t, a[:, 0], a[:, 1:1 + vals]


def nif_keys(r, kind, force=False):
    """kind: number of floats per value; 'q' = quaternion"""
    return read_keys(r, kind, force)[0]


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
        b["next"] = r.i32(); r.u16(); r.skip(16); b["target"] = r.i32()

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
            controller(b); b["data"] = r.i32()
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
            b["next"] = r.i32(); r.i32(); k = r.i32()
            b["keys"] = [(r.f(), r.string()) for _ in range(k)]
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
            rt, b["rt"], b["rv"] = read_keys(r, "q")
            if rt == 4:
                r.f(); b["rxyz"] = [read_keys(r, 1, True)[1:] for _ in range(3)]
            _, b["tt"], b["tv"] = read_keys(r, 3)
            _, b["st"], b["sv"] = read_keys(r, 1)
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
            b["bones"] = []
            for _ in range(k):
                rot = r.fs(9).reshape(3, 3); tr = r.fs(3); sc = r.f(); r.skip(16)
                w = r.u16()
                raw = np.frombuffer(r.d, np.dtype([("i", "<u2"), ("w", "<f4")]), w, r.p); r.skip(6 * w)
                b["bones"].append((rot, tr, sc, raw["i"].astype(np.int64), raw["w"].astype(np.float64)))
        elif typ == "NiSkinInstance":
            b["data"] = r.i32(); b["root"] = r.i32(); b["bones"] = r.reflist()
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


# Texture formats in textures.bin / the cell's terrain texture (the runtime maps them to GPU_TEXCOLOR)
FMT_RGB565, FMT_RGBA4, FMT_ETC1, FMT_ETC1A4 = 0, 1, 2, 3
ETC = True   # --rgb turns it off: 16-bit textures as before
LOD = True   # --no-lod turns it off: no lighter copies of big things
TEX3DS = r"C:\devkitPro\tools\bin\tex3ds.exe"


def bleed(img):
    """Gives the fully transparent texels the colour of the visible ones nearby (8 rounds of averaging the
    neighbours, wrapping like a repeating texture; the rest: the average colour). Otherwise they are black
    and filtering and mipmaps blend black into the edges of leaves. They get alpha 1, not 0: tex3ds blackens
    alpha-0 texels, and 1 still encodes as 0 in ETC1A4's 4-bit alpha."""
    a = np.asarray(img).astype(np.float32)
    clear = a[..., 3] == 0
    if not clear.any() or clear.all():
        return img
    rgb = a[..., :3].copy()
    known = ~clear
    for _ in range(8):
        if known.all():
            break
        s = np.zeros_like(rgb); n = np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                k = np.roll(known, (dy, dx), (0, 1))
                s += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
                n += k
        grow = ~known & (n > 0)
        rgb[grow] = s[grow] / n[grow][:, None]
        known |= grow
    rgb[~known] = a[..., :3][~clear].mean(axis=0)
    out = a.copy()
    out[..., :3] = np.where(clear[..., None], rgb, a[..., :3])
    out[..., 3] = np.where(clear, 1.0, a[..., 3])
    return Image.fromarray(np.clip(np.rint(out), 0, 255).astype(np.uint8), "RGBA")


def encode_etc(img, fmt):
    """ETC1 (4 bpp) or ETC1A4 (8 bpp) with all mipmaps, by devkitPro's tex3ds. tex3ds keeps image row 0 at
    t = 1, so it gets the image flipped: then its output is laid out like to_3ds's (checked byte for byte on
    RGB565)."""
    if fmt == FMT_ETC1A4:
        img = bleed(img)
    src = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    with tempfile.TemporaryDirectory(prefix="mwbake_") as tmp:
        png = os.path.join(tmp, "in.png"); out = os.path.join(tmp, "out.bin")
        (src if fmt == FMT_ETC1A4 else src.convert("RGB")).save(png)
        r = subprocess.run([TEX3DS, "-f", "etc1a4" if fmt == FMT_ETC1A4 else "etc1", "-q", "medium", "-m", "box",
                            "-z", "none", "-r", "-o", out, png], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("tex3ds: " + r.stderr.strip())
        blob = open(out, "rb").read()
    data = blob[4:]   # -r still writes a 4-byte header (compression type 0, size)
    w, h = img.size
    levels = 0; expect = 0; bpp = 8 if fmt == FMT_ETC1A4 else 4
    while True:
        expect += w * h * bpp // 8; levels += 1
        if w <= 8 or h <= 8:
            break
        w //= 2; h //= 2
    if expect != len(data):
        raise RuntimeError("tex3ds: %d bytes, expected %d" % (len(data), expect))
    return data, levels


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
                    if SATURATION != 1 or CONTRAST != 1:
                        a = img.getchannel("A")
                        rgb = ImageEnhance.Color(img.convert("RGB")).enhance(SATURATION)
                        rgb = ImageEnhance.Contrast(rgb).enhance(CONTRAST)
                        img = rgb.convert("RGBA"); img.putalpha(a)
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
        if ETC:
            # encoded later, all at once (write): tex3ds runs in parallel
            has_alpha = alpha and np.asarray(im)[..., 3].min() < 250
            self.list.append([w, h, FMT_ETC1A4 if has_alpha else FMT_ETC1, None, im])
        else:
            fmt = 1 if alpha else 0
            data, levels = to_3ds(im, fmt)
            self.list.append((w, h, fmt, levels, data))
        self.index[key] = len(self.list) - 1
        return self.index[key]

    def write(self, path):
        pending = [e for e in self.list if isinstance(e, list)]
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            for e, (data, levels) in zip(pending, ex.map(lambda e: encode_etc(e[4], e[2]), pending)):
                e[3] = levels; e[4] = data
        with open(path, "wb") as f:
            f.write(b"MWT1" + struct.pack("<I", len(self.list)))
            for w, h, fmt, levels, data in self.list:
                f.write(struct.pack("<HHBBHI", w, h, fmt, levels, 0, len(data)))
                f.write(data)

    def bytes(self):
        return sum(len(e[4]) for e in self.list)

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
        if LOD:
            for s in shapes:
                s["lod"] = lod_shape(s)
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
    if ETC:
        data, levels = encode_etc(img, FMT_ETC1)
        return data, levels, FMT_ETC1
    data, levels = to_3ds(img, 0)
    return data, levels, FMT_RGB565


def point_light(v, nrm, lights):
    """the lamps of an interior on vertices (cell-relative): sum of colour x (1 - d / radius) x facing"""
    out = np.zeros((len(v), 3))
    for p, r, c in lights:
        d = p[None, :] - v
        dist = np.maximum(np.linalg.norm(d, axis=1), 1e-3)
        att = np.clip(1.0 - dist / r, 0, 1)
        if not att.any():
            continue
        face = np.ones(len(v)) if nrm is None else np.clip((nrm * d).sum(axis=1) / dist, 0, 1) * 0.75 + 0.25
        out += c[None, :] * (att * face)[:, None]
    return out * 1.3


def bake_cell(esm, models, tex, gx, gy, stats, dyn=None, inter=None):
    """inter: an interior put at this grid cell ({"refs": shifted refs, "lights": [(pos, radius, rgb)]}):
    no terrain, no water, its own light; every wide upward face is a floor (stairs, tables...)"""
    ox, oy = gx * CELL + HALF, gy * CELL + HALF
    batches = []    # list of Batch
    land = esm.lands.get((gx, gy)) if inter is None else None
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
    boxes = []
    solids = []    # (triangles (n, 3, 3) cell-relative) of the things that block the way
    floors = []    # wide upward triangles of the things walked on (bridges, docks, stairs)
    steps = []     # wide upward triangles of everything else: floors only where reached from the ground
    for ref in (esm.cells.get((gx, gy), []) if inter is None else inter["refs"]):
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
        otype = obj_type(esm.kind.get(ref["id"]))
        if otype == OT_CONT and ref["id"] in esm.organic:
            otype = 0
        if otype and dyn is not None:
            # a dynamic object: its own record (objects.bin), not merged; doors and containers still block the way
            dyn.append((ref, otype, gx, gy))
            if otype != OT_ITEM:
                solids.append(np.concatenate([(sh["v"] @ R.T + np.array([lx, ly, pz]))[sh["tri"]] for sh in shapes]))
            continue
        sub = (0 if lx < 0 else 1) + (0 if ly < 0 else 2)
        # size class (docs/POMYSLY.md, step 1): small things (grass, kelp, shells: radius < 180) get their own
        # batches, drawn only near; things wholly under the water too (seen from the land only close by)
        allv = np.concatenate([s["v"] for s in shapes])
        rad = float(np.linalg.norm(allv, axis=1).max()) * ref["scale"]
        top = pz + float((allv @ R.T)[:, 2].max())
        cls = (8 if rad < SMALL_RADIUS else 0) | (16 if top < 0 else 0)
        walk = inter is not None or any(w in ref["id"] for w in WALKABLE)
        # plants: walked through (the user: they held the character). Grass, ferns, bushes, kelp: every
        # flora_ thing but trees, stumps, logs and roots; and any small thing with see-through leaves.
        rid = ref["id"]
        plant = ((cls & 8) and any(sh["alpha"] for sh in shapes)) or (
            rid.startswith("flora_") and not any(k in rid for k in ("tree", "stump", "log", "root", "rock", "hollow")))
        if not (cls & 16) and not plant:
            tv = np.concatenate([(sh["v"] @ R.T + np.array([lx, ly, pz]))[sh["tri"]] for sh in shapes])
            if walk:
                fl = floor_mask(tv)
                floors.append(tv[fl]); solids.append(tv[~fl])
            else:
                solids.append(tv)
                steps.append(tv[floor_mask(tv, 0.72)])   # gentler than 44 degrees (Morrowind's sMaxSlope: 46)
        stats["small_tris" if cls & 8 else "under_tris" if cls & 16 else "big_tris"] += sum(len(s["tri"]) for s in shapes)
        for s0 in shapes:
            # big things with a lighter LOD copy: the full part near (flag 32), the copy farther (flag 64)
            lod = s0.get("lod")
            if cls == 0 and lod is not None and lod is not s0:
                variants = ((s0, 32), (lod, 64))
                stats["lod_tris"] += len(lod["tri"]); stats["lod_full_tris"] += len(s0["tri"])
            else:
                variants = ((s0, 0),)
            for s, lodflag in variants:
                v = s["v"] @ R.T + np.array([lx, ly, pz])
                nrm = None if s["n"] is None else s["n"] @ Rn.T
                rgb = light(nrm, s["base"], s["emi"])
                if inter is not None and inter["lights"]:
                    rgb = np.clip(rgb + s["base"] * point_light(v, nrm, inter["lights"]), 0, 1)
                col = np.concatenate([rgb * 255, s["a"][:, None] * 255], axis=1)
                col = np.clip(col, 0, 255).astype(np.uint8)
                ti = tex.get(s["tex"], s["alpha"])
                flags = (1 if s["alpha"] else 0) | (2 if s["double"] else 0) | cls | lodflag
                key = (sub, ti, flags)
                bl = groups.get(key)
                if bl is None or bl[-1].nv + len(v) > 65000:
                    bl = groups.setdefault(key, [])
                    bl.append(Batch(ti, flags))
                bl[-1].add(v, s["uv"], col, s["tri"])
            stats["object_tris"] += len(s0["tri"])
    for key in sorted(groups):
        batches.extend(groups[key])
    if inter is None and (land is None or minh < 0):
        # water at z = 0
        bt = Batch(0, 4, 150)
        p = np.array([[-HALF, -HALF, 0], [HALF, -HALF, 0], [HALF, HALF, 0], [-HALF, HALF, 0]])
        col = np.array([[50, 85, 105, 255]] * 4, np.uint8)
        bt.add(p, np.zeros((4, 2)), col, np.array([[0, 1, 2], [0, 2, 3]]))
        batches.append(bt)
    stats["refs"] += nrefs
    flo = np.concatenate(floors) if floors else np.zeros((0, 3, 3))
    stp = np.concatenate(steps) if steps else np.zeros((0, 3, 3))
    occ, occU, reached = occupancy(esm, gx, gy, solids, flo, stp)
    stats["reached"] = stats.get("reached", 0) + len(reached)
    flo = np.concatenate([flo, reached])
    stats["floors"] = stats.get("floors", 0) + len(flo)
    stats["blocked"] = stats.get("blocked", 0) + int(occ.sum())
    stats["upper"] = stats.get("upper", 0) + int((occU != occ).sum())
    return batches, terrain_tex, (flo, occ, occU)


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


# Collision (docs/POMYSLY.md): per thing, its footprint where a body walks: the triangles that
# reach into 30..140 units over the ground (a trunk, not the crown; walls, not the eaves), as a
# box turned like the thing. Floors to walk on (docks, bridges, planks) have none.
WALKABLE = ("dock", "bridge", "pier", "plank", "walkway", "skywalk", "_plat", "stair", "steps", "ramp", "rug", "_floor", "boardwalk")
BAND = (30.0, 140.0)


def collision_box(shapes, R, lx, ly, pz, gz):
    pts = []
    zlo = zhi = None
    for s in shapes:
        v = s["v"] @ R.T + np.array([lx, ly, pz])
        tz = v[s["tri"]][:, :, 2]
        hit = (tz.max(axis=1) > gz + BAND[0]) & (tz.min(axis=1) < gz + BAND[1])
        if not hit.any():
            continue
        tv = v[s["tri"][hit]].reshape(-1, 3)
        pts.append(tv)
        a, b = float(tv[:, 2].min()), float(tv[:, 2].max())
        zlo = a if zlo is None else min(zlo, a); zhi = b if zhi is None else max(zhi, b)
    if not pts:
        return None
    pts = np.concatenate(pts)
    ax = R[:2, 0]
    n = float(np.hypot(*ax))
    ca, sa = (ax[0] / n, ax[1] / n) if n > 1e-6 else (1.0, 0.0)
    u = pts[:, 0] * ca + pts[:, 1] * sa
    w = -pts[:, 0] * sa + pts[:, 1] * ca
    hu, hw = (u.max() - u.min()) / 2, (w.max() - w.min()) / 2
    if hu < 6 and hw < 6:
        return None
    cu, cw = (u.max() + u.min()) / 2, (w.max() + w.min()) / 2
    cx, cy = cu * ca - cw * sa, cu * sa + cw * ca
    return (cx, cy, ca, sa, hu, hw, zlo, zhi)


# Collision, version 2 (the boxes held the character in empty places: a whole L of a wall, or grass):
# a grid of 16-unit squares per cell, a square blocked where a thing reaches into the body's band
# (30..140 units over where one stands: the ground or a floor). Floors: the upward faces of the
# things walked on that are wide enough to stand on (not a railing's top), stood on at run time.
OCC = 512          # squares per cell side (16 units)
FLOOR_NZ = 0.6     # floors slope less than ~53 degrees
FLOOR_WIDTH = 12.0 # ... and are wider than this (twice the area / the longest side)


def floor_mask(tv, min_nz=FLOOR_NZ):
    e0 = tv[:, 1] - tv[:, 0]; e1 = tv[:, 2] - tv[:, 0]
    n = np.cross(e0, e1); ln = np.linalg.norm(n, axis=1)
    nz = np.where(ln > 1e-9, n[:, 2] / np.maximum(ln, 1e-9), 0)
    longest = np.maximum(np.maximum(np.linalg.norm(e0, axis=1), np.linalg.norm(e1, axis=1)),
                         np.linalg.norm(tv[:, 2] - tv[:, 1], axis=1))
    width = ln / np.maximum(longest, 1e-9)
    return (nz > min_nz) & (width > FLOOR_WIDTH)


def sample_tris(tv, spacing=8.0, ids=False):
    """points over the triangles, at most `spacing` apart (and with ids: each point's triangle)"""
    if not len(tv):
        return (np.zeros((0, 3)), np.zeros(0, int)) if ids else np.zeros((0, 3))
    longest = np.maximum(np.maximum(np.linalg.norm(tv[:, 1] - tv[:, 0], axis=1), np.linalg.norm(tv[:, 2] - tv[:, 0], axis=1)),
                         np.linalg.norm(tv[:, 2] - tv[:, 1], axis=1))
    k = np.clip(np.ceil(longest / spacing), 1, 96).astype(int)
    out = []; oid = []
    for kk in np.unique(k):
        tid = np.nonzero(k == kk)[0]
        sel = tv[tid]
        a, b = np.meshgrid(np.arange(kk + 1), np.arange(kk + 1))
        m = (a + b) <= kk
        a = a[m] / kk; b = b[m] / kk
        p = sel[:, None, 0] + a[None, :, None] * (sel[:, None, 1] - sel[:, None, 0]) + b[None, :, None] * (sel[:, None, 2] - sel[:, None, 0])
        out.append(p.reshape(-1, 3)); oid.append(np.repeat(tid, len(a)))
    return (np.concatenate(out), np.concatenate(oid)) if ids else np.concatenate(out)


# Stairs and flat roofs that are part of a building (Balmora: steps up to a door, roofs one walks on):
# an upward face counts as a floor only where it is reached from the ground (or a floor) by steps of
# at most STEP_UP between neighbouring squares, so a roof with no stairs to it stays a roof.
STEP_UP = 36.0     # OpenMW's sStepSizeUp is 34 (apps/openmw/mwphysics/constants.hpp)
STEP_LAYERS = 8


def reach_floors(surf, stp, sq):
    """surf raised where the steps' faces are reached; the reached triangles"""
    if not len(stp):
        return surf, np.zeros((0, 3, 3))
    p, tid = sample_tris(stp, 6.0, ids=True)
    sx = np.floor((p[:, 0] + HALF) / sq).astype(int); sy = np.floor((p[:, 1] + HALF) / sq).astype(int)
    ok = (sx >= 0) & (sx < OCC) & (sy >= 0) & (sy < OCC)
    p, tid, sx, sy = p[ok], tid[ok], sx[ok], sy[ok]
    ok = p[:, 2] > surf[sy, sx] + 2.0
    p, tid, sx, sy = p[ok], tid[ok], sx[ok], sy[ok]
    if not len(p):
        return surf, np.zeros((0, 3, 3))
    # the candidate heights per square: the lowest STEP_LAYERS distinct ones (4-unit steps)
    sqi = sy * OCC + sx
    zi = np.clip(np.round(p[:, 2] / 4.0).astype(np.int64) + 32768, 0, 65535)
    k1 = np.unique(sqi.astype(np.int64) * 65536 + zi)
    key = np.stack([k1 // 65536, (k1 % 65536 - 32768) * 4.0], 1)
    first = np.r_[0, np.nonzero(np.diff(key[:, 0]))[0] + 1]
    start = np.repeat(first, np.diff(np.r_[first, len(key)]))
    rank = np.arange(len(key)) - start
    key = key[rank < STEP_LAYERS]; rank = rank[rank < STEP_LAYERS]
    # only the region round the candidates
    ki = key[:, 0].astype(int)
    y0, y1 = max(ki.min() // OCC - 1, 0), min(ki.max() // OCC + 2, OCC)
    xs = ki % OCC
    x0, x1 = max(xs.min() - 1, 0), min(xs.max() + 2, OCC)
    cand = np.full((y1 - y0, x1 - x0, STEP_LAYERS), np.nan)
    cand[ki // OCC - y0, xs - x0, rank] = key[:, 1]
    # a graph: the candidates are nodes, joined where neighbouring squares' heights differ by at most
    # STEP_UP; a component is reached when one of its nodes is that near the ground (its own square's
    # or a neighbour's)
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    H, W, L = cand.shape
    nid = np.arange(H * W * L).reshape(H, W, L)
    base = surf[y0:y1, x0:x1]
    ea, eb = [], []
    seed = np.abs(cand - base[:, :, None]) <= STEP_UP
    for dy, dx in ((1, 0), (0, 1)):
        A = cand[:H - dy, :W - dx]; B = cand[dy:, dx:]
        ia = nid[:H - dy, :W - dx]; ib = nid[dy:, dx:]
        for i in range(L):
            for j in range(L):
                m = np.abs(A[:, :, i] - B[:, :, j]) <= STEP_UP
                ea.append(ia[:, :, i][m]); eb.append(ib[:, :, j][m])
        # a node next to the ground (either way)
        seed[:H - dy, :W - dx] |= np.abs(A - base[dy:, dx:, None]) <= STEP_UP
        seed[dy:, dx:] |= np.abs(B - base[:H - dy, :W - dx, None]) <= STEP_UP
    ea = np.concatenate(ea); eb = np.concatenate(eb)
    g = coo_matrix((np.ones(len(ea), np.int8), (ea, eb)), shape=(H * W * L, H * W * L))
    _, lab = connected_components(g, directed=False)
    ok = np.zeros(lab.max() + 1, bool); ok[lab[seed.ravel()]] = True
    reach = ok[lab].reshape(H, W, L) & ~np.isnan(cand)
    s = np.maximum(base, np.where(reach, cand, -1e9).max(-1))
    out = surf.copy(); out[y0:y1, x0:x1] = s
    hit = np.abs(p[:, 2] - out[sy, sx]) <= 3.0
    return out, stp[np.unique(tid[hit])]


def occupancy(esm, gx, gy, solids, flo, stp=np.zeros((0, 3, 3))):
    """the blocked squares (OCC x OCC bools, row = y) for one on the ground (or a walkable thing's floor),
    the same for one on reached steps / roofs (None when no different), and the reached step triangles"""
    sq = 2 * HALF / OCC
    land = esm.lands.get((gx, gy))
    cs = (np.arange(OCC) + 0.5) * sq / 128.0     # square centres in the terrain's 128-unit grid
    if land is not None and "h" in land:
        h = land["h"]
        ix = np.minimum(cs.astype(int), 63); u = cs - ix
        X, Y = np.meshgrid(ix, ix); U, V = np.meshgrid(u, u)
        surf = (h[Y, X] * (1 - U) * (1 - V) + h[Y, X + 1] * U * (1 - V) + h[Y + 1, X] * (1 - U) * V + h[Y + 1, X + 1] * U * V)
    else:
        surf = np.full((OCC, OCC), -2000.0)
    surf = surf.astype(np.float64)
    if len(flo):
        p = sample_tris(flo)
        sx = np.floor((p[:, 0] + HALF) / sq).astype(int); sy = np.floor((p[:, 1] + HALF) / sq).astype(int)
        ok = (sx >= 0) & (sx < OCC) & (sy >= 0) & (sy < OCC) & (p[:, 2] > surf[np.clip(sy, 0, OCC - 1), np.clip(sx, 0, OCC - 1)] - 10)
        fl = np.full((OCC, OCC), -1e9)
        np.maximum.at(fl, (sy[ok], sx[ok]), p[ok, 2])
        surf = np.maximum(surf, fl)
    surfU, reached = reach_floors(surf, stp, sq)
    occ = block(solids, surf, sq)
    occU = block(solids, surfU, sq) if len(reached) else occ
    return occ, occU, reached


def block(solids, surf, sq):
    occ = np.zeros((OCC, OCC), bool)
    for tv in solids:
        if not len(tv): continue
        p = sample_tris(tv)
        sx = np.floor((p[:, 0] + HALF) / sq).astype(int); sy = np.floor((p[:, 1] + HALF) / sq).astype(int)
        ok = (sx >= 0) & (sx < OCC) & (sy >= 0) & (sy < OCC)
        sx, sy, z = sx[ok], sy[ok], p[ok, 2]
        dz = z - surf[sy, sx]
        hit = (dz > BAND[0]) & (dz < BAND[1])
        occ[sy[hit], sx[hit]] = True
    return occ


def write_cell(path, gx, gy, batches, terrain_tex, tsize, coll=None):
    with open(path, "wb") as f:
        hdr = struct.Struct("<4siiHHIIIIIIII")
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
        colofs = f.tell()   # (MWC2's boxes: none now)
        flofs, nflo, occofs, occUofs = 0, 0, 0, 0
        if coll is not None:
            flo, occ, occU = coll
            align(f)
            flofs = f.tell(); nflo = len(flo)
            f.write(np.asarray(flo, "<f4").reshape(-1, 9).tobytes())
            align(f)
            occofs = f.tell()
            f.write(np.packbits(occ, axis=1, bitorder="little").tobytes())
            if occU is not occ:
                # for one on reached steps / roofs (the old ncol field, unused in MWC3)
                align(f)
                occUofs = f.tell()
                f.write(np.packbits(occU, axis=1, bitorder="little").tobytes())
        end = f.tell()
        f.seek(bofs); f.write(b"".join(recs))
        # tsize: the terrain texture's side, its format in the top 4 bits
        tword = tsize | (terrain_tex[2] << 12 if terrain_tex is not None else 0)
        f.seek(0); f.write(hdr.pack(b"MWC3", gx, gy, len(batches), tword, ttofs, bofs, end, colofs, occUofs, flofs, nflo, occofs))


def write_objects(path, esm, models, tex, dyn, shifts=None):
    """objects.bin (docs/PLAN.md step 1): the dynamic objects of the baked cells, each with its persistent id
    (plugin, FRMR), and their models once each (shared by every copy).

      header  "MWO1", u32 nbase, nmodel, npart, nobj, baseofs, modelofs, partofs, objofs, strofs, strbytes
      base    nbase x (u32 id string ofs, u32 name string ofs, u16 type (1 item, 2 door, 3 container), u16 model)
      model   nmodel x (u16 first part, u16 parts, f32 quant (position = int16 x quant), f32 radius)
      part    npart x (u16 texture, u8 flags (1 alpha, 2 two-sided), u8 0, u32 vofs, u32 vcount, u32 iofs, u32 icount)
      object  nobj x (u32 frmr, u16 plugin, u16 base, s16 gx, s16 gy, f32 pos[3], f32 rot[3], f32 scale,
              u32 destination string ofs: a load door's interior ("" = outside), 0xFFFFFFFF not a load door,
              f32 dest[4]: where it puts you (x y z, facing; moved like the baked interiors))  60 bytes
      strings UTF-8, NUL-terminated
      data    vertices (the cells' 16-byte format, model space, light baked as the model stands) and indices
    The light is baked for the model unturned (good enough for the things lying round; a turned door is lit as if not)."""
    bases, base_ix, mdl_ix = [], {}, {}
    for ref, otype, gx, gy in dyn:
        if ref["id"] not in base_ix:
            mp = esm.models[ref["id"]].lower()
            if mp not in mdl_ix: mdl_ix[mp] = len(mdl_ix)
            base_ix[ref["id"]] = len(bases)
            bases.append((ref["id"], esm.names.get(ref["id"], ref["id"]), otype, mdl_ix[mp]))
    strs = bytearray(); sofs = {}
    def S(t):
        if t not in sofs:
            sofs[t] = len(strs); strs.extend(t.encode("utf-8") + b"\0")
        return sofs[t]
    base_rec = b"".join(struct.pack("<IIHH", S(i), S(n), t, m) for i, n, t, m in bases)
    blob = bytearray(); model_rec = []; part_rec = []
    def put(b):
        while len(blob) % 16: blob.append(0)
        o = len(blob); blob.extend(b); return o
    for mp, mi in sorted(mdl_ix.items(), key=lambda kv: kv[1]):
        shapes = models.get(mp) or []
        allv = np.concatenate([s["v"] for s in shapes]) if shapes else np.zeros((1, 3))
        ext = float(np.abs(allv).max()) if len(allv) else 1.0
        q = max(ext / 32000.0, 1.0 / 64.0)
        rad = float(np.linalg.norm(allv, axis=1).max()) if len(allv) else 1.0
        first = len(part_rec)
        for s in shapes:
            rgb = light(s["n"], s["base"], s["emi"])
            col = np.clip(np.concatenate([rgb * 255, s["a"][:, None] * 255], axis=1), 0, 255).astype(np.uint8)
            vtx = np.zeros(len(s["v"]), dtype=[("p", "<i2", 4), ("t", "<i2", 2), ("c", "u1", 4)])
            vtx["p"][:, :3] = np.clip(np.round(s["v"] / q), -32767, 32767)
            vtx["t"] = np.clip(np.round(s["uv"] * UV_SCALE), -32767, 32767)
            vtx["c"] = col
            tri = s["tri"].astype("<u2")
            vo = put(vtx.tobytes()); io_ = put(tri.tobytes())
            flags = (1 if s["alpha"] else 0) | (2 if s["double"] else 0)
            part_rec.append(struct.pack("<HBBIIII", tex.get(s["tex"], s["alpha"]), flags, 0, vo, len(s["v"]), io_, tri.size))
        model_rec.append(struct.pack("<HHff", first, len(part_rec) - first, q, rad))
    obj_rec = []
    for ref, otype, gx, gy in dyn:
        px, py, pz, rx, ry, rz = ref["pos"]
        dest = S(ref["dest"]) if "dest" in ref else 0xFFFFFFFF
        dp = list(ref.get("destpos", (0, 0, 0, 0, 0, 0)))
        sh = (shifts or {}).get(ref.get("dest"))
        if sh:
            dp[0] += sh[0]; dp[1] += sh[1]; dp[2] += sh[2]
        obj_rec.append(struct.pack("<IHHhh7fI4f", ref["frmr"], 0, base_ix[ref["id"]], gx, gy, px, py, pz, rx, ry, rz, ref["scale"], dest,
                                   dp[0], dp[1], dp[2], dp[5]))
    hdr = struct.Struct("<4s10I")
    o = hdr.size
    def nxt(n):
        nonlocal o
        start = o; o += n; return start
    baseofs = nxt(len(base_rec)); modelofs = nxt(12 * len(model_rec)); partofs = nxt(20 * len(part_rec))
    objofs = nxt(60 * len(obj_rec)); strofs = nxt(len(strs))
    o = (o + 15) & ~15
    dataofs = o
    # the parts' offsets are relative to the data block: made absolute here
    fixed = []
    for pr in part_rec:
        t, fl, z, vo, vc, io_, ic = struct.unpack("<HBBIIII", pr)
        fixed.append(struct.pack("<HBBIIII", t, fl, z, vo + dataofs, vc, io_ + dataofs, ic))
    with open(path, "wb") as f:
        f.write(hdr.pack(b"MWO3", len(bases), len(model_rec), len(part_rec), len(obj_rec),
                         baseofs, modelofs, partofs, objofs, strofs, len(strs)))
        f.write(base_rec); f.write(b"".join(model_rec)); f.write(b"".join(fixed)); f.write(b"".join(obj_rec)); f.write(strs)
        f.write(b"\0" * (dataofs - f.tell()))
        f.write(blob)
    kinds = {}
    for _, t, _, _ in dyn: kinds[t] = kinds.get(t, 0) + 1
    print("load doors: %d of %d" % (sum(1 for r, t, _, _ in dyn if t == OT_DOOR and "dest" in r), kinds.get(OT_DOOR, 0)))
    print("objects.bin: %d objects (items %d, doors %d, containers %d), %d kinds, %d models, %d parts, %.0f KB" % (
        len(obj_rec), kinds.get(OT_ITEM, 0), kinds.get(OT_DOOR, 0), kinds.get(OT_CONT, 0), len(bases), len(model_rec),
        len(part_rec), os.path.getsize(path) / 1024))


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--data", default=r"D:\Programowanie\OpenMorrowind\data\Data Files")
    ap.add_argument("--center", type=int, nargs=2, default=[-2, -9], help="start cell (Seyda Neen: -2 -9)")
    ap.add_argument("--rect", type=int, nargs=4, default=[-5, -11, 2, -1],
                    help="cells to bake: x0 y0 x1 y1 (default: Seyda Neen to Pelagiad and Balmora)")
    ap.add_argument("--radius", type=int, default=None, help="instead of --rect: a square round --center")
    ap.add_argument("--start", type=float, nargs=2, default=[-11008.0, -71040.0],
                    help="where the game starts (an open square in Seyda Neen, the user: not at a house corner)")
    ap.add_argument("--route-to", type=int, nargs=2, default=[0, -7],
                    help="the benchmark walks from --center's cell to this one (Pelagiad: 0 -7)")
    ap.add_argument("--tex", type=int, default=128, help="max object texture size")
    ap.add_argument("--saturation", type=float, default=1.0, help="texture colour boost (1 = none)")
    ap.add_argument("--contrast", type=float, default=1.0, help="texture contrast boost (1 = none)")
    ap.add_argument("--terrain-tex", type=int, default=1024, help="composite terrain texture per cell")
    ap.add_argument("--no-char", action="store_true", help="no romfs/char.bin")
    ap.add_argument("--interiors", nargs="*", default=["Seyda Neen, Arrille's Tradehouse", "Seyda Neen, Vodunius Nuccius' House"],
                    help="interior cells to bake (by name), each at its own far grid cell")
    ap.add_argument("--no-objects", action="store_true", help="items, doors, containers merged into the statics (no objects.bin)")
    ap.add_argument("--no-lod", action="store_true", help="no LOD copies (docs/POMYSLY.md)")
    ap.add_argument("--rgb", action="store_true", help="16-bit textures (RGB565 / RGBA4) instead of ETC1 / ETC1A4")
    ap.add_argument("--out", default=os.path.join(here, "..", "romfs"))
    a = ap.parse_args()
    global SATURATION, CONTRAST
    SATURATION, CONTRAST = a.saturation, a.contrast
    global ETC, LOD
    ETC = not a.rgb
    LOD = not a.no_lod
    if a.rgb and a.terrain_tex > 256:
        a.terrain_tex = 256

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
    stats = {"terrain_tris": 0, "object_tris": 0, "refs": 0, "terrain_tex": a.terrain_tex, "small_tris": 0, "under_tris": 0, "big_tris": 0, "lod_tris": 0, "lod_full_tris": 0}
    cx, cy = a.center
    total_bytes = 0
    if a.radius is not None:
        x0, y0, x1, y1 = cx - a.radius, cy - a.radius, cx + a.radius, cy + a.radius
    else:
        x0, y0, x1, y1 = a.rect
    sizes = {}
    dyn = [] if not a.no_objects else None
    for gy in range(y0, y1 + 1):
        for gx in range(x0, x1 + 1):
            if (gx, gy) not in esm.lands and (gx, gy) not in esm.cells:
                continue
            before = dict(stats)
            batches, ttex, coll = bake_cell(esm, models, tex, gx, gy, stats, dyn)
            p = os.path.join(a.out, "cells", "%d_%d.bin" % (gx, gy))
            write_cell(p, gx, gy, batches, ttex, a.terrain_tex, coll)
            sz = os.path.getsize(p); total_bytes += sz; sizes[(gx, gy)] = sz
            done.append((gx, gy))
            print("cell %3d,%3d: %4d refs, %6d object tris, %5d terrain tris, %3d batches, %6.0f KB" % (
                gx, gy, stats["refs"] - before["refs"], stats["object_tris"] - before["object_tris"],
                stats["terrain_tris"] - before["terrain_tris"], len(batches), sz / 1024))
    # interiors (as Morrowind: their own cells, entered by load doors): each put at its own grid cell far off the
    # map (100 + k, 100), centred, the floor at ~1000 (no sea under it); interiors.bin names them
    shifts = {}
    inter_rec = []
    for k, name in enumerate(a.interiors):
        it = esm.interiors.get(name)
        if not it or not it["refs"]:
            print("interior %r: not found" % name); continue
        gx, gy = 100 + k, 100
        P = np.array([r["pos"][:3] for r in it["refs"]])
        sh = (gx * CELL + HALF - float((P[:, 0].min() + P[:, 0].max()) / 2),
              gy * CELL + HALF - float((P[:, 1].min() + P[:, 1].max()) / 2), 1000.0 - float(P[:, 2].min()))
        shifts[name] = sh
        refs = []
        for r in it["refs"]:
            r2 = dict(r); p = r["pos"]
            r2["pos"] = (p[0] + sh[0], p[1] + sh[1], p[2] + sh[2], p[3], p[4], p[5])
            refs.append(r2)
        ox, oy = gx * CELL + HALF, gy * CELL + HALF
        lights = [(np.array([r["pos"][0] - ox, r["pos"][1] - oy, r["pos"][2]]), *esm.lights[r["id"]]) for r in refs if r["id"] in esm.lights]
        amb, sun, fog = it["ambi"] or (np.array([0.3, 0.3, 0.3]), np.array([0.3, 0.3, 0.3]), np.array([0.1, 0.1, 0.1]))
        global AMBIENT, SUNCOL
        keep = (AMBIENT, SUNCOL)
        AMBIENT, SUNCOL = np.clip(amb * 1.15 + 0.06, 0, 1), sun * 0.5
        before = dict(stats)
        batches, ttex, coll = bake_cell(esm, models, tex, gx, gy, stats, dyn, {"refs": refs, "lights": lights})
        AMBIENT, SUNCOL = keep
        p = os.path.join(a.out, "cells", "%d_%d.bin" % (gx, gy))
        write_cell(p, gx, gy, batches, None, a.terrain_tex, coll)
        sz = os.path.getsize(p); total_bytes += sz; sizes[(gx, gy)] = sz
        done.append((gx, gy))
        fc = (np.clip(fog, 0, 1) * 255).astype(int)
        inter_rec.append(struct.pack("<iiI64s", gx, gy, int(fc[0]) | int(fc[1]) << 8 | int(fc[2]) << 16, name.encode("utf-8")[:63]))
        print("interior %r -> cell %d,%d: %d refs, %d lamps, %d object tris, %d batches, %.0f KB" % (
            name, gx, gy, stats["refs"] - before["refs"], len(lights), stats["object_tris"] - before["object_tris"], len(batches), sz / 1024))
    with open(os.path.join(a.out, "interiors.bin"), "wb") as f:
        f.write(b"MWN1" + struct.pack("<I", len(inter_rec)) + b"".join(inter_rec))
    if not a.no_char:
        import chrbake
        ch = chrbake.Character(vfs, os.path.join(a.data, "Morrowind.esm"), tex)
        ch.write(os.path.join(a.out, "char.bin"))
        print("char.bin: %.0f KB" % (os.path.getsize(os.path.join(a.out, "char.bin")) / 1024))
        import uibake
        uibake.bake(vfs, os.path.join(a.data, "Morrowind.esm"), ch, a.out)
    try:
        import sndbake
        sndbake.bake(a.data, a.out)
    except Exception as ex:   # (miniaudio missing: no sound)
        print("sounds not baked: %s" % ex)
    print("collision: %d floor triangles (%d of them reached steps / roofs), %d blocked squares, %d other up there" % (
        stats.get("floors", 0), stats.get("reached", 0), stats.get("blocked", 0), stats.get("upper", 0)))
    objp = os.path.join(a.out, "objects.bin")
    if dyn is not None:
        write_objects(objp, esm, models, tex, dyn, shifts)
    elif os.path.exists(objp):
        os.remove(objp)
    tex.write(os.path.join(a.out, "textures.bin"))
    # start: the centre cell, above the ground
    sx, sy = a.start
    gz = ground(esm, sx, sy)
    # the benchmark's route: a straight line from the start cell's centre to --route-to's centre, a point every
    # 256 units at eye height over the ground (or the water)
    tx, ty = a.route_to[0] * CELL + HALF, a.route_to[1] * CELL + HALF
    rx0, ry0 = cx * CELL + HALF, cy * CELL + HALF   # (the route as before: from the centre cell's middle)
    steps = max(2, int(math.hypot(tx - rx0, ty - ry0) / 256))
    route = []
    for i in range(steps + 1):
        x = rx0 + (tx - rx0) * i / steps; y = ry0 + (ty - ry0) * i / steps
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
    if stats["lod_full_tris"]:
        print("LOD: parts with a copy %d tris -> %d (%.0f %%)" % (stats["lod_full_tris"], stats["lod_tris"], 100.0 * stats["lod_tris"] / stats["lod_full_tris"]))
    print("textures: %d (%.0f KB), cells %.0f KB, models parsed %d, failed %d, missing textures %d, %.1fs" % (
        len(tex.list), tbytes / 1024, total_bytes / 1024, len(models.cache), len(models.failed), len(tex.missing), time.time() - t0))
    for k, v in list(models.failed.items())[:20]:
        print("  model %s: %s" % (k, v))
    for k in list(tex.missing)[:10]:
        print("  missing texture %s" % k)


if __name__ == "__main__":
    main()
