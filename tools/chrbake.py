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
# (name, text-key group, walks?, (first key, last key) or None for the whole group / its loop, loops?)
ANIMS = [("idle", "Idle", False, None, True), ("walk", "WalkForward", True, None, True),
         ("run", "RunForward", True, None, True),
         ("swimidle", "IdleSwim", False, None, True), ("swimwalk", "SwimWalkForward", True, None, True),
         ("swimrun", "SwimRunForward", True, None, True),
         ("jump", "Jump", False, ("start", "stop"), False),
         ("idle1h", "Idle1h", False, None, True), ("walk1h", "WalkForward1h", True, None, True),
         ("run1h", "RunForward1h", True, None, True),
         # Morrowind's three attacks: the swing up to its hit, then ONE follow-through (it has a small,
         # medium and large one after each other in the file; all three played looked like 3 attacks)
         ("slash1h", "WeaponOneHand", False, [("slash start", "slash hit"), ("slash large follow start", "slash large follow stop")], False),
         ("thrust1h", "WeaponOneHand", False, [("thrust start", "thrust hit"), ("thrust large follow start", "thrust large follow stop")], False),
         ("chop1h", "WeaponOneHand", False, [("chop start", "chop hit"), ("chop large follow start", "chop large follow stop")], False),
         ("slashhh", "HandToHand", False, [("slash start", "slash hit"), ("slash large follow start", "slash large follow stop")], False),
         ("thrusthh", "HandToHand", False, [("thrust start", "thrust hit"), ("thrust large follow start", "thrust large follow stop")], False),
         ("chophh", "HandToHand", False, [("chop start", "chop hit"), ("chop large follow start", "chop large follow stop")], False),
         ("hit", "Hit1", False, ("start", "stop"), False),
         ("death", "Death1", False, ("start", "stop"), False)]
# weapons for the right hand (WEAP ids; missing ones skipped), hung on the "Weapon Bone"
WEAPONS = ["iron longsword", "iron saber", "steel longsword", "iron war axe", "steel axe", "iron club",
           "steel mace", "chitin club", "glass longsword", "ebony longsword", "daedric longsword"]
MAX_WEAPONS = 6
WEAPON_SLOT = 25
MAXB = 20          # bones per draw (the skinning shader's palette)
TORCH_MODEL = "l/light_com_torch_01.nif"
TORCH_SLOT = 10      # "Shield Bone": carried lights hang there (OpenMW: the left hand's carried item)

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
        elif tag == b"WEAP":
            d = {}
            for n, v in M.subrecords(body):
                if n == b"NAME": d["id"] = M.cstr(v).lower()
                elif n == b"MODL": d["model"] = M.cstr(v)
            if "id" in d and "model" in d:
                items["weap:" + d["id"]] = d
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

    def subtree(self, name):
        root = self.by_name.get(name.lower())
        out = set()
        for ni, (bi, nm, parent) in enumerate(self.nodes):
            if ni == root or parent in out: out.add(ni)
        return out

    def pose(self, t=None, strip_root=True, over=None):
        """every node's matrix in the character's space; over = (nodes, time): those nodes' own
        motion from another time (a layered animation: the arm with the torch)"""
        out = [None] * len(self.nodes)
        for ni, (bi, name, parent) in enumerate(self.nodes):
            L = self.local(ni, over[1], strip_root) if over and ni in over[0] else self.local(ni, t, strip_root)
            out[ni] = L if parent < 0 else out[parent] @ L
        return out

    def keys(self, name, k0, k1):
        """the times of two text keys of a group ("chop start" .. "chop small follow stop")"""
        a = b = None
        for t, txt in self.text:
            for line in txt.replace("\r", "").split("\n"):
                line = line.strip().lower()
                if line == (name + ": " + k0).lower(): a = t
                elif line == (name + ": " + k1).lower(): b = t
        return a, b

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
        # for the GPU: the vertices as in the NIF and per bone the matrix into that bone's space
        self.vpos = None; self.vnrm = None; self.bm = {}


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
                    m.bm[ni] = Bm
                    for vi, wt in zip(idx, w):
                        if wt <= 0.001 or vi >= nv: continue
                        p = Bm[:3, :3] @ d["v"][vi] + Bm[:3, 3]
                        n = rot @ nrm[vi]
                        m.infl[vi].append((ni, float(wt), p, n))
            else:
                L = mirror @ W
                m.bm[attach] = L
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
            m.vpos = np.asarray(d["v"], float); m.vnrm = np.asarray(nrm, float)
            out.append(m)
            return
        for c in b.get("children", []):
            walk(c, W, pr, depth + 1)

    walk(0, np.eye(4), {}, 0)
    return out


def split_mesh(m):
    """meshes using more than MAXB bones cut into pieces of at most MAXB (by triangles)"""
    usedb = {e[0] for inf in m.infl for e in inf}
    m.bm = {k: v for k, v in m.bm.items() if k in usedb}
    for ni in usedb - set(m.bm):   # (an influence without a matrix: the fallback to the attach node)
        m.bm[ni] = np.eye(4)
    if len(m.bm) <= MAXB:
        return [m]
    out = []
    tris = list(m.tri)
    while tris:
        bones, take, rest = set(), [], []
        for t in tris:
            tb = {e[0] for vi in t for e in m.infl[vi]}
            if len(bones | tb) <= MAXB: bones |= tb; take.append(t)
            else: rest.append(t)
        if not take: break
        vs = sorted({int(v) for t in take for v in t}); rm = {v: k for k, v in enumerate(vs)}
        q = Mesh(m.tex, m.alpha, m.colour)
        q.uv = np.asarray(m.uv)[vs]; q.infl = [m.infl[v] for v in vs]
        q.vpos = m.vpos[vs]; q.vnrm = m.vnrm[vs]
        q.bm = {ni: m.bm[ni] for ni in bones if ni in m.bm}
        q.tri = np.array([[rm[int(v)] for v in t] for t in take], np.int64)
        out.append(q); tris = rest
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
                ms = [q for m0 in part_meshes(vfs, tex, self.sk, model, slot, warn) for q in split_mesh(m0)]
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
        # weapons
        self.weapons = []   # (name, group)
        for wid in WEAPONS:
            w = items.get("weap:" + wid)
            if w is None:
                warn("no weapon " + wid); continue
            g = group_for(w["model"], WEAPON_SLOT)
            if g >= 0: self.weapons.append((wid, g))
            if len(self.weapons) >= MAX_WEAPONS: break
        log("  weapons: %s" % ", ".join(n for n, g in self.weapons))
        # the torch: a rigid part on the shield bone; its flame where the NIF's "AttachLight" is
        self.torch = group_for(TORCH_MODEL, TORCH_SLOT)
        self.flame = np.array([0.0, 0.0, 30.0])
        tb = M.parse_nif(vfs.read("meshes\\" + TORCH_MODEL.replace("/", "\\")))
        def find(i, Mx, depth=0):
            if not (0 <= i < len(tb)) or depth > 64: return None
            b = tb[i]
            if "r" not in b: return None
            W = Mx @ trafo(b["r"], b["t"], b["s"])
            if b.get("name", "").lower() == "attachlight": return W[:3, 3]
            for c in b.get("children", []):
                r = find(c, W, depth + 1)
                if r is not None: return r
            return None
        fp = find(0, np.eye(4))
        if fp is not None: self.flame = fp
        # Hung on the shield bone as it is, the torch points out sideways in the "Torch" pose: turned
        # (in the bone's space) to point up and a little forward there.
        ta, tb_ = self.sk.group("Torch")
        if self.torch >= 0 and ta is not None:
            sbn = self.sk.by_name.get(SLOT_NODE[TORCH_SLOT].lower())
            arm = self.sk.subtree("Bip01 L Clavicle")
            P = self.sk.pose(ta, over=(arm, ta + (tb_ - ta) * 0.5))
            Rb = P[sbn][:3, :3]; Rb = Rb / np.linalg.norm(Rb[:, 0])
            want = np.array([0.0, 0.45, 1.0]); want /= np.linalg.norm(want)
            v = Rb.T @ want
            z = self.flame / max(np.linalg.norm(self.flame), 1e-6)
            ax = np.cross(z, v); sn = np.linalg.norm(ax); cs = float(z @ v)
            if sn > 1e-6:
                ax /= sn; K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
                Rf = np.eye(3) + sn * K + (1 - cs) * K @ K
                R4 = np.eye(4); R4[:3, :3] = Rf
                for mi in self.groups[self.torch]:
                    m = self.meshes[mi]
                    m.infl = [[(a, w, Rf @ p_, Rf @ n_) for a, w, p_, n_ in lst] for lst in m.infl]
                    m.bm = {k: R4 @ v for k, v in m.bm.items()}
                self.flame = Rf @ self.flame
        log("  torch: group %d, flame at %s" % (self.torch, np.round(self.flame, 1)))
        # the bones the meshes use
        used = {e[0] for m in self.meshes for inf in m.infl for e in inf}
        neck = self.sk.by_name.get("bip01 neck")
        if neck is not None: used.add(neck)
        used = sorted(used)
        self.bones = used
        self.bone_index = {ni: i for i, ni in enumerate(used)}
        # the animations
        self.anims = []
        for short, group, moves, keys, loop in ANIMS:
            # the pieces of the group to play one after the other
            segs = keys if isinstance(keys, list) else [keys] if keys else [None]
            frames = []
            ok = True
            hit_t = 0.0
            for si, sg in enumerate(segs):
                a, b = self.sk.keys(group, *sg) if sg else self.sk.group(group)
                if a is None or b is None:
                    ok = False; break
                n = max(2, int(round((b - a) * FPS)))
                last = 0 if loop else 1
                for k in range(n + last):   # a one-off ends on its last key
                    P = self.sk.pose(a + (b - a) * k / n, strip_root=True)
                    frames.append(np.stack([P[ni][:3, :4] for ni in used]))
                if si == 0 and len(segs) > 1: hit_t = len(frames) / FPS   # the hit: the end of the swing
            if not ok:
                warn("no animation " + group + " " + str(keys)); continue
            a, b = self.sk.keys(group, *segs[0]) if segs[0] else self.sk.group(group)
            speed = 0.0
            if moves:   # how fast the animation walks: the root's travel over the loop
                bip = self.sk.by_name.get("bip01")
                p0 = self.sk.pose(a, strip_root=False)[bip][:3, 3]
                p1 = self.sk.pose(b, strip_root=False)[bip][:3, 3]
                speed = float(np.linalg.norm((p1 - p0)[:2]) / max(b - a, 1e-6))
            if not moves and hit_t > 0:
                speed = hit_t   # (one-off attacks: the field holds the time of the hit, seconds)
            self.anims.append((short, np.stack(frames), speed, loop))
            log("  animation %s: %.2f s, %d frames, %.0f units/s%s" % (short, b - a, len(frames), speed, "" if loop else ", once"))
        # the torch held up: the left arm's bones from the "Torch" pose, laid on whatever plays (at run
        # time, relative to the neck)
        arm = self.sk.subtree("Bip01 L Clavicle")
        self.arm_mask = [1 if ni in arm else 0 for ni in used]
        self.neck_index = self.bone_index.get(neck, 0)
        self.torch_pose = None
        if ta is not None:
            P = self.sk.pose(ta, strip_root=True, over=(arm, ta + (tb_ - ta) * 0.5))
            self.torch_pose = np.stack([P[ni][:3, :4] for ni in used])
        nv = sum(len(m.infl) for m in self.meshes)
        log("  %d meshes in %d groups, %d vertices, %d bones used, %d items, %d warnings" % (
            len(self.meshes), len(self.groups), nv, len(used), len(self.items), len(self.warnings)))
        for w in self.warnings[:15]:
            log("    " + w)

    def write(self, path):
        """char.bin "MWCK" (GPU skinning; see main.c loadCharacter):
          u32 nbones, nanims, nmeshes, ngroups, nitems, noutfits, nweapons
          anims:   name[16], u32 frames, f32 fps, f32 speed, u32 loops; frames x nbones x 12 f32
          meshes:  u16 tex, u8 alpha, u8 npal, u32 nverts, nidx, f32 colour[3]
                   npal x { u8 bone, 3 pad, f32 m[12] } (the NIF's mesh space into that bone's)
                   nverts x { f32 pos[3], s16 nrm[4] (x 32767), s16 uv[2] (x 1024), u8 pal[4] (x 3), u8 w[4] (x 255) }
                   nidx x u16, padded to 4
          groups, body, items, outfits: as before
          weapons: name[24], s16 group, 2 pad
          "TRCH": s16 group, u8 bone, u8 neck, f32 flame[3], nbones x u8 arm mask (padded to 4),
                  nbones x 12 f32 (the torch pose)"""
        with open(path, "wb") as f:
            f.write(b"MWCK" + struct.pack("<7I", len(self.bones), len(self.anims), len(self.meshes),
                                          len(self.groups), len(self.items), len(self.outfits), len(self.weapons)))
            for name, frames, speed, loop in self.anims:
                f.write(name.encode()[:15].ljust(16, b"\0") + struct.pack("<IffI", len(frames), FPS, speed, 1 if loop else 0))
                f.write(frames.astype("<f4").tobytes())
            vt = np.dtype([("p", "<f4", 3), ("n", "<i2", 4), ("t", "<i2", 2), ("i", "u1", 4), ("w", "u1", 4)])
            for m in self.meshes:
                pal = sorted(m.bm)
                slot = {ni: k for k, ni in enumerate(pal)}
                f.write(struct.pack("<HBBII3f", m.tex, m.alpha, len(pal), len(m.vpos), m.tri.size, *m.colour))
                for ni in pal:
                    f.write(struct.pack("<B3x12f", self.bone_index[ni], *m.bm[ni][:3, :4].ravel()))
                v = np.zeros(len(m.vpos), vt)
                v["p"] = m.vpos
                v["n"][:, :3] = np.clip(np.round(m.vnrm * 32767), -32767, 32767)
                v["t"] = np.clip(np.round(np.asarray(m.uv) * 1024), -32767, 32767)
                for vi, lst in enumerate(m.infl):
                    tot = 0
                    for k, (ni, w, _, _) in enumerate(lst[:4]):
                        v["i"][vi, k] = slot.get(ni, 0) * 3
                        q = int(round(w * 255)); v["w"][vi, k] = q; tot += q
                    v["w"][vi, 0] = max(0, min(255, int(v["w"][vi, 0]) + 255 - tot))   # the weights sum to 255
                f.write(v.tobytes())
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
                for slot_, g in parts:
                    f.write(struct.pack("<BBh", slot_, 0, g))
                f.write(b"\0\0")
            for name, lst in self.outfits:
                f.write(name.encode()[:15].ljust(16, b"\0") + struct.pack("<B", len(lst)) + bytes(lst))
                pad = (-(17 + len(lst))) % 4
                f.write(b"\0" * pad)
            for name, g in self.weapons:
                f.write(name.encode()[:23].ljust(24, b"\0") + struct.pack("<hxx", g))
            tbone = self.bone_index.get(self.sk.by_name.get(SLOT_NODE[TORCH_SLOT].lower()), 0)
            f.write(b"TRCH" + struct.pack("<hBB3f", self.torch, tbone, self.neck_index, *self.flame))
            f.write(bytes(self.arm_mask) + b"\0" * ((-len(self.arm_mask)) % 4))
            tp = self.torch_pose if self.torch_pose is not None else self.anims[0][1][0]
            f.write(tp.astype("<f4").tobytes())

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
