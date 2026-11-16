"""The character for the 3DS: Morrowind's skeleton and animations (meshes/base_anim.nif), a race's
body parts and sets of clothes / armour, baked into romfs/char.bin. Called from mwbake.py (it
shares the texture store), or alone with --preview to draw the result into PNGs.

How a character is put together (after OpenMW's npcanimation.cpp and sceneutil/attach.cpp):
  27 slots (head, hair, neck, cuirass, groin, skirt, hands, wrists, forearms, upper arms, feet,
  ankles, knees, upper legs, pauldrons (clavicles), shield, weapon, tail), each tied to a node of
  the skeleton ("Head", "Chest", "Left Forearm", ...). A slot shows the race's body part, unless
  something worn covers it (an ARMO / CLOT record lists the slots it covers and the BODY part for
  each, or none: then the slot is just hidden).
  A part's NIF is either skinned (its shapes are weighted to bones by name; only the shapes named
  "Tri <slot's node>" are taken, the hair's: "hair"), or rigid: it is hung on the slot's node, and
  on the left side mirrored (x -> -x).

char.bin ("MWCH", little-endian):
  u32 nbones, nanims, nmeshes, ngroups, nitems, noutfits
  anims:   char name[16], u32 frames, f32 fps, f32 speed (units/s the animation walks at, 0 idle),
           frames x nbones x 12 f32 (the bones' 3x4 matrices in the character's space, rows)
  meshes:  u16 tex, u8 alpha, u8 pad, u32 nverts, ninfl, nidx, f32 colour[3]
           nverts x { s16 u, v (x1024), u8 ninfl, u8 pad[3] }
           ninfl x { u8 bone, u8 pad[3], f32 weight, f32 pos[3] (in the bone's space), f32 nrm[3] }
           nidx x u16, padded to 4 bytes
  groups:  u16 n, n x u16 mesh       (what one slot shows: one or more meshes)
  body:    27 x s16 group            (the bare body per slot, -1 none)
  items:   char name[24], u8 helmet, u8 n, n x { u8 slot, u8 pad, s16 group (-1: hidden) }
  outfits: char name[16], u8 n, n x u8 item
"""
import argparse, math, os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mwbake as M

SLOT_NODE = ["Head", "Head", "Neck", "Chest", "Groin", "Groin", "Right Hand", "Left Hand", "Right Wrist",
             "Left Wrist", "Shield Bone", "Right Forearm", "Left Forearm", "Right Upper Arm", "Left Upper Arm",
             "Right Foot", "Left Foot", "Right Ankle", "Left Ankle", "Right Knee", "Left Knee",
             "Right Upper Leg", "Left Upper Leg", "Right Clavicle", "Left Clavicle", "Weapon Bone", "Tail"]
NSLOT = 27
# BODY part (BYDT) -> slots
PART_SLOTS = {0: [0], 1: [1], 2: [2], 3: [3], 4: [4], 5: [6, 7], 6: [8, 9], 7: [11, 12], 8: [13, 14],
              9: [15, 16], 10: [17, 18], 11: [19, 20], 12: [21, 22], 13: [23, 24], 14: [26]}

FPS = 20.0
ANIMS = [("idle", "Idle", False), ("walk", "WalkForward", True), ("run", "RunForward", True)]

# what the character can wear: outfits of ARMO / CLOT ids (missing ones are skipped)
OUTFITS = [
    ("bare", []),
    ("commoner", ["common_shirt_01", "common_pants_01", "common_shoes_01"]),
    ("netch", ["netch_leather_cuirass", "netch_leather_greaves", "netch_leather_boots",
               "netch_leather_pauldron_left", "netch_leather_pauldron_right", "netch_leather_gauntlet_left",
               "netch_leather_gauntlet_right", "netch_leather_boiled_helm"]),
    ("iron", ["iron_cuirass", "iron_greaves", "iron boots", "iron_pauldron_left", "iron_pauldron_right",
              "iron_gauntlet_left", "iron_gauntlet_right", "iron_helmet"]),
    ("bonemold", ["bonemold_cuirass", "bonemold_greaves", "bonemold_boots", "bonemold_pauldron_l",
                  "bonemold_pauldron_r", "bonemold_bracer_left", "bonemold_bracer_right", "bonemold_helm"]),
    ("glass", ["glass_cuirass", "glass_greaves", "glass_boots", "glass_pauldron_left", "glass_pauldron_right",
               "glass_bracer_left", "glass_bracer_right", "glass_helm"]),
]


# --------------------------------------------------------------------------- ESM: bodies, armour, clothes

def load_records(path):
    data = open(path, "rb").read()
    bodies, items = {}, {}
    p = 0
    while p + 16 <= len(data):
        tag = data[p:p + 4]
        size = struct.unpack_from("<I", data, p + 4)[0]
        body = data[p + 16:p + 16 + size]
        p += 16 + size
        if tag == b"BODY":
            d = {}
            for n, v in M.subrecords(body):
                if n == b"NAME": d["id"] = M.cstr(v).lower()
                elif n == b"MODL": d["model"] = M.cstr(v)
                elif n == b"FNAM": d["race"] = M.cstr(v).lower()
                elif n == b"BYDT": d["part"], d["vamp"], d["flags"], d["type"] = v[0], v[1], v[2], v[3]
            if "id" in d:
                bodies[d["id"]] = d
        elif tag in (b"ARMO", b"CLOT"):
            d = {"parts": []}
            cur = None
            for n, v in M.subrecords(body):
                if n == b"NAME": d["id"] = M.cstr(v).lower()
                elif n == b"INDX": cur = [v[0], None]; d["parts"].append(cur)
                elif n == b"BNAM" and cur is not None: cur[1] = M.cstr(v).lower()
            if "id" in d:
                items[d["id"]] = d
    return bodies, items


# --------------------------------------------------------------------------- maths

def trafo(R, t, s):
    L = np.eye(4); L[:3, :3] = R * s; L[:3, 3] = t
    return L


def quat_mat(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def slerp(a, b, f):
    d = float(np.dot(a, b))
    if d < 0: b = -b; d = -d
    if d > 0.9995:
        q = a + (b - a) * f
    else:
        th = math.acos(d); s = math.sin(th)
        q = (math.sin((1 - f) * th) * a + math.sin(f * th) * b) / s
    return q / np.linalg.norm(q)


def sample(times, vals, t):
    """index and fraction of t between keys"""
    if len(times) == 1 or t <= times[0]: return 0, 0, 0.0
    if t >= times[-1]: n = len(times) - 1; return n, n, 0.0
    i = int(np.searchsorted(times, t, side="right")) - 1
    f = (t - times[i]) / max(times[i + 1] - times[i], 1e-9)
    return i, i + 1, f


# --------------------------------------------------------------------------- skeleton and animations

class Skeleton:
    def __init__(self, blocks, transpose_quats):
        self.b = blocks
        self.tq = transpose_quats
        self.nodes = []     # (block index, name, parent node index)
        self.by_name = {}
        self.ctrl = {}      # node index -> NiKeyframeData block
        self._walk(0, -1)
        self.text = []
        for x in blocks:
            if x["type"] == "NiTextKeyExtraData":
                self.text = x["keys"]

    def _walk(self, bi, parent):
        x = self.b[bi]
        if x["type"] not in ("NiNode",):
            return
        ni = len(self.nodes)
        self.nodes.append((bi, x["name"], parent))
        self.by_name.setdefault(x["name"].lower(), ni)
        c = x.get("ctrl", -1)
        while 0 <= c < len(self.b):
            cb = self.b[c]
            if cb["type"] == "NiKeyframeController" and 0 <= cb.get("data", -1) < len(self.b):
                self.ctrl[ni] = self.b[cb["data"]]
                break
            c = cb.get("next", -1)
        for ch in x["children"]:
            if 0 <= ch < len(self.b):
                self._walk(ch, ni)

    def local(self, ni, t=None, strip_root=False):
        x = self.b[self.nodes[ni][0]]
        R, tr, s = x["r"], x["t"].copy(), x["s"]
        k = self.ctrl.get(ni) if t is not None else None
        if k is not None:
            if k.get("rv") is not None and len(k["rv"]):
                i, j, f = sample(k["rt"], k["rv"], t)
                q = slerp(k["rv"][i], k["rv"][j], f)
                R = quat_mat(q)
                if self.tq: R = R.T
            elif "rxyz" in k:
                ang = []
                for times, vals in k["rxyz"]:
                    if times is None or not len(times): ang.append(0.0); continue
                    i, j, f = sample(times, vals, t)
                    ang.append(float(vals[i][0] * (1 - f) + vals[j][0] * f))
                cx, cy, cz = (math.cos(a) for a in ang); sx, sy, sz = (math.sin(a) for a in ang)
                Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
                Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
                Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
                R = Rz @ Ry @ Rx
                if self.tq: R = R.T
            if k.get("tv") is not None and len(k["tv"]):
                i, j, f = sample(k["tt"], k["tv"], t)
                tr = k["tv"][i] * (1 - f) + k["tv"][j] * f
                if strip_root and self.nodes[ni][1].lower() == "bip01":
                    tr = np.array([x["t"][0], x["t"][1], tr[2]])   # the walk stays on the spot (root motion out)
            if k.get("sv") is not None and len(k["sv"]):
                i, j, f = sample(k["st"], k["sv"], t)
                s = float(k["sv"][i][0] * (1 - f) + k["sv"][j][0] * f)
        return trafo(R, tr, s)

    def pose(self, t=None, strip_root=True):
        """every node's matrix in the character's space"""
        out = [None] * len(self.nodes)
        for ni, (bi, name, parent) in enumerate(self.nodes):
            L = self.local(ni, t, strip_root)
            out[ni] = L if parent < 0 else out[parent] @ L
        return out

    def group(self, name):
        start = stop = lstart = lstop = None
        for t, txt in self.text:
            for line in txt.replace("\r", "").split("\n"):
                line = line.strip().lower()
                n = name.lower() + ":"
                if not line.startswith(n): continue
                what = line[len(n):].strip()
                if what == "start": start = t
                elif what == "stop": stop = t
                elif what == "loop start": lstart = t
                elif what == "loop stop": lstop = t
        if lstart is not None and lstop is not None:
            return lstart, lstop
        return start, stop


def quat_convention(blocks):
    """NIF quaternions vs. the node matrices: which way round (rotation or its transpose) matches
    the bind pose better at the first key."""
    sk = Skeleton(blocks, False)
    err = [0.0, 0.0]
    for ni, k in sk.ctrl.items():
        if k.get("rv") is None or not len(k["rv"]): continue
        Rb = blocks[sk.nodes[ni][0]]["r"]
        Rq = quat_mat(k["rv"][0])
        err[0] += np.abs(Rq - Rb).sum(); err[1] += np.abs(Rq.T - Rb).sum()
    return err[1] < err[0], err


# --------------------------------------------------------------------------- parts

class Mesh:
    def __init__(self, tex, alpha, colour):
        self.tex, self.alpha, self.colour = tex, alpha, colour
        self.uv = []; self.infl = []; self.tri = None   # infl: per vertex a list of (bone node, w, pos, nrm)


def part_meshes(vfs, tex, sk, model, slot, warn):
    """The meshes a part shows in a slot, influences in skeleton-node terms."""
    data = vfs.read("meshes\\" + model.replace("/", "\\"))
    if data is None:
        warn("missing " + model); return []
    try:
        blocks = M.parse_nif(data)
    except Exception as ex:
        warn("%s: %s" % (model, ex)); return []
    skinned = any(b["type"] == "NiTriShape" and b.get("skin", -1) >= 0 for b in blocks)
    node = SLOT_NODE[slot]
    filt = "hair" if slot == 1 else node.lower()
    attach = sk.by_name.get(node.lower())
    if attach is None:
        warn("no node " + node); return []
    mirror = np.diag([-1.0, 1, 1, 1]) if (not skinned and "left" in node.lower()) else np.eye(4)
    out = []

    def walk(i, Mx, props, depth):
        if not (0 <= i < len(blocks)) or depth > 64: return
        b = blocks[i]
        if b["type"] in ("RootCollisionNode", "NiBSParticleNode", "NiCamera"): return
        if "flags" not in b or (b["flags"] & 1): return
        W = Mx @ trafo(b["r"], b["t"], b["s"])
        pr = dict(props)
        for p in b.get("props", []):
            if 0 <= p < len(blocks): pr[blocks[p]["type"]] = blocks[p]
        if b["type"] == "NiTriShape":
            name = b["name"].lower()
            if skinned:
                if b.get("skin", -1) < 0: return
                if not (name.startswith(filt) or name.startswith("tri " + filt)): return
            d = blocks[b["data"]] if 0 <= b["data"] < len(blocks) else None
            if not d or d["type"] != "NiTriShapeData" or d["v"] is None or not len(d["tri"]): return
            texname, alpha, double, dif, emi, vmode = M.material_of(pr, blocks)
            ti = tex.get(texname, alpha) if texname else 0xFFFF
            m = Mesh(ti, alpha, np.clip(dif + emi, 0, 1))
            nv = len(d["v"])
            nrm = d["n"] if d["n"] is not None else np.tile([0, 0, 1.0], (nv, 1))
            m.uv = d["uv"] if d["uv"] is not None else np.zeros((nv, 2))
            m.infl = [[] for _ in range(nv)]
            if skinned:
                si = blocks[b["skin"]]; sd = blocks[si["data"]]
                for bref, (rot, tr, sc, idx, w) in zip(si["bones"], sd["bones"]):
                    bname = blocks[bref]["name"].lower() if 0 <= bref < len(blocks) else ""
                    ni = sk.by_name.get(bname)
                    if ni is None:
                        warn("%s: bone %s not in the skeleton" % (model, bname)); continue
                    Bm = trafo(rot, tr, sc)
                    for vi, wt in zip(idx, w):
                        if wt <= 0.001 or vi >= nv: continue
                        p = Bm[:3, :3] @ d["v"][vi] + Bm[:3, 3]
                        n = rot @ nrm[vi]
                        m.infl[vi].append((ni, float(wt), p, n))
            else:
                L = mirror @ W
                for vi in range(nv):
                    p = L[:3, :3] @ d["v"][vi] + L[:3, 3]
                    n = L[:3, :3] @ nrm[vi]; n = n / max(np.linalg.norm(n), 1e-6)
                    m.infl[vi].append((attach, 1.0, p, n))
            # at most 4 influences, weights summing to 1
            for vi in range(nv):
                lst = sorted(m.infl[vi], key=lambda e: -e[1])[:4]
                if not lst:
                    lst = [(attach, 1.0, np.zeros(3), np.array([0, 0, 1.0]))]
                tot = sum(e[1] for e in lst)
                m.infl[vi] = [(a, w / tot, p, n) for a, w, p, n in lst]
            m.tri = d["tri"].astype(np.int64)
            out.append(m)
            return
        for c in b.get("children", []):
            walk(c, W, pr, depth + 1)

    walk(0, np.eye(4), {}, 0)
    return out


# --------------------------------------------------------------------------- the whole character

class Character:
    def __init__(self, vfs, esm_path, tex, race="dark elf", head="b_n_dark elf_m_head_01",
                 hair="b_n_dark elf_m_hair_01", log=print):
        self.log = log
        self.warnings = []
        warn = lambda s: self.warnings.append(s)
        blocks = M.parse_nif(vfs.read("meshes\\base_anim.nif"))
        tq, err = quat_convention(blocks)
        self.sk = Skeleton(blocks, tq)
        log("character: skeleton %d nodes, %d animated, quaternions %s (%.1f / %.1f)" % (
            len(self.sk.nodes), len(self.sk.ctrl), "transposed" if tq else "as they are", err[0], err[1]))
        bodies, items = load_records(esm_path)
        self.meshes = []    # Mesh
        self.groups = []    # lists of mesh indices
        cache = {}

        def group_for(model, slot):
            key = (model.lower(), slot)
            if key not in cache:
                ms = part_meshes(vfs, tex, self.sk, model, slot, warn)
                if not ms:
                    cache[key] = -1
                else:
                    ids = []
                    for m in ms:
                        ids.append(len(self.meshes)); self.meshes.append(m)
                    cache[key] = len(self.groups); self.groups.append(ids)
            return cache[key]

        # the bare body: the race's male skin parts (not 1st person, not vampire), the chosen head and hair
        self.body = [-1] * NSLOT
        for b in bodies.values():
            if b.get("race") != race or b.get("type") != 0 or b.get("flags", 0) & 1 or b.get("vamp"): continue
            if b["id"].endswith(".1st") or b.get("part") in (0, 1): continue
            for slot in PART_SLOTS.get(b["part"], []):
                if self.body[slot] < 0:
                    self.body[slot] = group_for(b["model"], slot)
        for slot, bid in ((0, head), (1, hair)):
            if bid in bodies: self.body[slot] = group_for(bodies[bid]["model"], slot)
        # items and outfits
        self.items = []     # (name, helmet, [(slot, group)])
        self.outfits = []   # (name, [item index])
        for oname, ids in OUTFITS:
            lst = []
            for iid in ids:
                it = items.get(iid)
                if it is None:
                    warn("no item " + iid); continue
                parts = []
                for slot, bname in it["parts"]:
                    if slot >= NSLOT: continue
                    g = -1
                    if bname and bname in bodies:
                        g = group_for(bodies[bname]["model"], slot)
                    parts.append((slot, g))
                helmet = 1 if any(s in (0, 1) for s, _ in parts) else 0
                lst.append(len(self.items)); self.items.append((iid, helmet, parts))
            self.outfits.append((oname, lst))
        # the bones the meshes use
        used = sorted({e[0] for m in self.meshes for inf in m.infl for e in inf})
        self.bones = used
        self.bone_index = {ni: i for i, ni in enumerate(used)}
        # the animations
        self.anims = []
        for short, group, moves in ANIMS:
            a, b = self.sk.group(group)
            if a is None or b is None:
                warn("no animation " + group); continue
            n = max(2, int(round((b - a) * FPS)))
            frames = []
            for k in range(n):
                P = self.sk.pose(a + (b - a) * k / n, strip_root=True)
                frames.append(np.stack([P[ni][:3, :4] for ni in used]))
            speed = 0.0
            if moves:   # how fast the animation walks: the root's travel over the loop
                bip = self.sk.by_name.get("bip01")
                p0 = self.sk.pose(a, strip_root=False)[bip][:3, 3]
                p1 = self.sk.pose(b, strip_root=False)[bip][:3, 3]
                speed = float(np.linalg.norm((p1 - p0)[:2]) / max(b - a, 1e-6))
            self.anims.append((short, np.stack(frames), speed))
            log("  animation %s: %.2f s, %d frames, %.0f units/s" % (short, b - a, n, speed))
        nv = sum(len(m.infl) for m in self.meshes)
        log("  %d meshes in %d groups, %d vertices, %d bones used, %d items, %d warnings" % (
            len(self.meshes), len(self.groups), nv, len(used), len(self.items), len(self.warnings)))
        for w in self.warnings[:15]:
            log("    " + w)

    def write(self, path):
        with open(path, "wb") as f:
            f.write(b"MWCH" + struct.pack("<6I", len(self.bones), len(self.anims), len(self.meshes),
                                          len(self.groups), len(self.items), len(self.outfits)))
            for name, frames, speed in self.anims:
                f.write(name.encode()[:15].ljust(16, b"\0") + struct.pack("<Iff", len(frames), FPS, speed))
                f.write(frames.astype("<f4").tobytes())
            for m in self.meshes:
                ninf = sum(len(i) for i in m.infl)
                f.write(struct.pack("<HBBIII3f", m.tex, m.alpha, 0, len(m.infl), ninf, m.tri.size, *m.colour))
                vt = bytearray()
                inf = bytearray()
                for vi, lst in enumerate(m.infl):
                    u, v = m.uv[vi]
                    vt += struct.pack("<hhB3x", int(np.clip(round(u * 1024), -32767, 32767)),
                                      int(np.clip(round(v * 1024), -32767, 32767)), len(lst))
                    for ni, w, p, n in lst:
                        inf += struct.pack("<B3x7f", self.bone_index[ni], w, *p, *n)
                f.write(vt); f.write(inf)
                idx = m.tri.astype("<u2").tobytes()
                f.write(idx)
                if len(idx) % 4: f.write(b"\0" * (4 - len(idx) % 4))
            for g in self.groups:
                f.write(struct.pack("<H%dH" % len(g), len(g), *g))
                if (len(g) + 1) % 2: f.write(b"\0\0")
            f.write(struct.pack("<%dh" % NSLOT, *self.body))
            f.write(b"\0\0")
            for name, helmet, parts in self.items:
                f.write(name.encode()[:23].ljust(24, b"\0") + struct.pack("<BB", helmet, len(parts)))
                for slot, g in parts:
                    f.write(struct.pack("<BBh", slot, 0, g))
                f.write(b"\0\0")
            for name, lst in self.outfits:
                f.write(name.encode()[:15].ljust(16, b"\0") + struct.pack("<B", len(lst)) + bytes(lst))
                pad = (-(17 + len(lst))) % 4
                f.write(b"\0" * pad)

    # ----------------------------------------------------------------------- preview (PC)

    def shown(self, outfit, helmet=True):
        slots = list(self.body)
        worn = [self.items[i] for i in self.outfits[outfit][1]]
        hide_hair = False
        for name, hel, parts in worn:
            if hel and not helmet: continue
            if hel: hide_hair = True
            for slot, g in parts:
                slots[slot] = g
        if hide_hair: slots[1] = -1
        gs = []
        for g in slots:
            if g >= 0 and g not in gs: gs.append(g)
        return [mi for g in gs for mi in self.groups[g]]

    def skin(self, mi, bones):
        m = self.meshes[mi]
        P = np.zeros((len(m.infl), 3)); Nn = np.zeros((len(m.infl), 3))
        for vi, lst in enumerate(m.infl):
            for ni, w, p, n in lst:
                B = bones[self.bone_index[ni]]
                P[vi] += w * (B[:, :3] @ p + B[:, 3]); Nn[vi] += w * (B[:, :3] @ n)
        return P, Nn


def preview(ch, tex, path, outfit=0, anim=0, frame=0, yaw=0.6, helmet=True, size=320):
    """A software drawing of the character (flat-lit triangles, painter's order) to check it."""
    from PIL import Image, ImageDraw
    bones = ch.anims[anim][1][frame]
    img = Image.new("RGB", (size, size), (140, 168, 196))
    dr = ImageDraw.Draw(img)
    tris = []
    cy, sy = math.cos(yaw), math.sin(yaw)
    light = np.array([-0.35, -0.6, 0.7]); light /= np.linalg.norm(light)
    for mi in ch.shown(outfit, helmet):
        P, Nn = ch.skin(mi, bones)
        m = ch.meshes[mi]
        X = P[:, 0] * cy - P[:, 1] * sy; D = P[:, 0] * sy + P[:, 1] * cy; Z = P[:, 2]
        base = np.array([200, 170, 150]) if m.tex == 0xFFFF else np.array(
            tex.avg.get(m.tex, (170, 150, 130)) if hasattr(tex, "avg") else (170, 150, 130))
        for a, b, c in m.tri:
            n = np.cross(P[b] - P[a], P[c] - P[a]); l = np.linalg.norm(n)
            k = 0.45 + 0.55 * abs(float(n @ light)) / l if l > 0 else 0.6
            col = tuple(int(v) for v in np.clip(base * k * m.colour, 0, 255))
            tris.append((D[a] + D[b] + D[c], [(X[i], Z[i]) for i in (a, b, c)], col))
    tris.sort(key=lambda e: -e[0])
    s = size / 160.0
    for _, pts, col in tris:
        dr.polygon([(size / 2 + x * s, size - 10 - z * s) for x, z in pts], fill=col)
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=r"D:\Programowanie\OpenMorrowind\data\Data Files")
    ap.add_argument("--preview", default=None, help="folder for PNG previews")
    a = ap.parse_args()
    vfs = M.VFS(a.data, ["Morrowind.bsa"])
    tex = M.TexStore(vfs, 128)
    ch = Character(vfs, os.path.join(a.data, "Morrowind.esm"), tex)
    if a.preview:
        os.makedirs(a.preview, exist_ok=True)
        for o in range(len(ch.outfits)):
            preview(ch, tex, os.path.join(a.preview, "outfit%d_%s.png" % (o, ch.outfits[o][0])), outfit=o)
        for ai in range(len(ch.anims)):
            n = len(ch.anims[ai][1])
            for k in (0, n // 4, n // 2, 3 * n // 4):
                preview(ch, tex, os.path.join(a.preview, "anim_%s_%02d.png" % (ch.anims[ai][0], k)),
                        outfit=1, anim=ai, frame=k, yaw=1.4)


if __name__ == "__main__":
    main()
