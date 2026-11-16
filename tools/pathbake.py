"""Morrowind's path grids (PGRD: the points the designers set in a cell and the links between them, what
the game's NPCs walk on) -> romfs/pathgrid.bin, one graph over every baked cell, in world coordinates
(an interior moved as mwbake moves it). The game's A* goes over these few points instead of the 16-unit
squares (the user: a path over the squares froze the game for seconds on docks and stairs).

Points of neighbouring cells (each cell's grid is on its own in Morrowind) are joined when they are near.

pathgrid.bin: "MWP1", u32 npoints, u32 nlinks; points: f32 x, y, z, u32 first link, u16 links, u16 cell
index; links: u16 point.

Called by mwbake.py; alone (python tools/pathbake.py) it needs no bake: it reads the cells baked in romfs.
"""
import os, struct, sys
import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import mwbake as M

JOIN = 260.0   # points of two cells nearer than this are joined


def read_pgrd(esm_path, interiors=()):
    """{(gx, gy) or interior name: (points (n, 3) relative, [(a, b)])} (an interior's DATA grid is not (0, 0):
    known by its name)"""
    data = open(esm_path, "rb").read()
    out = {}
    p = 0
    while p + 16 <= len(data):
        tag = data[p:p + 4]
        size = struct.unpack_from("<I", data, p + 4)[0]
        body = data[p + 16:p + 16 + size]
        p += 16 + size
        if tag != b"PGRD":
            continue
        gx = gy = None; name = ""; pts = None; conn = None
        for n, d in M.subrecords(body):
            if n == b"DATA": gx, gy = struct.unpack("<ii", d[:8])
            elif n == b"NAME": name = M.cstr(d)
            elif n == b"PGRP": pts = np.frombuffer(d[:len(d) // 16 * 16], dtype=[("p", "<i4", 3), ("auto", "u1"), ("n", "u1"), ("pad", "<u2")])
            elif n == b"PGRC": conn = np.frombuffer(d[:len(d) // 4 * 4], "<u4")
        if pts is None or not len(pts):
            continue
        links = []
        k = 0
        for i, c in enumerate(pts["n"].tolist()):
            for j in conn[k:k + c] if conn is not None else []:
                if j < len(pts): links.append((i, int(j)))
            k += c
        out[name if name in interiors else (gx, gy)] = (pts["p"].astype(np.float64), links)
    return out


def bake(esm_path, out_dir, cells, shifts, log=print):
    """cells: the baked exterior (gx, gy); shifts: interior name -> (dx, dy, dz) as mwbake moved it"""
    grids = read_pgrd(esm_path, set(shifts))
    P, L, C = [], [], []
    ci = 0
    for key in list(cells) + list(shifts):
        g = grids.get(key)
        if g is None:
            continue
        pts, links = g
        if isinstance(key, tuple):
            off = np.array([key[0] * M.CELL, key[1] * M.CELL, 0.0])
        else:
            off = np.array(shifts[key])
        base = len(P)
        P.extend(list(pts + off))
        C.extend([ci] * len(pts))
        L.extend((base + a, base + b) for a, b in links)
        ci += 1
    if not P:
        log("pathgrid: none")
        return
    P = np.array(P); C = np.array(C)
    # the cells' grids joined where their points are near
    joined = 0
    for i in range(len(P)):
        d = np.linalg.norm(P - P[i], axis=1)
        for j in np.nonzero((d < JOIN) & (C != C[i]))[0]:
            if abs(P[j, 2] - P[i, 2]) < 60:
                L.append((i, int(j))); joined += 1
    adj = [set() for _ in range(len(P))]
    for a, b in L:
        if a != b:
            adj[a].add(b); adj[b].add(a)   # (Morrowind's links go both ways in practice)
    firsts, flat = [], []
    for s in adj:
        firsts.append(len(flat)); flat.extend(sorted(s))
    with open(os.path.join(out_dir, "pathgrid.bin"), "wb") as f:
        f.write(b"MWP1" + struct.pack("<II", len(P), len(flat)))
        for i in range(len(P)):
            f.write(struct.pack("<fffIHH", P[i, 0], P[i, 1], P[i, 2], firsts[i], len(adj[i]), int(C[i])))
        f.write(np.array(flat, "<u2").tobytes())
    log("pathgrid: %d grids, %d points, %d links (%d joining cells)" % (ci, len(P), len(flat), joined))


def main():
    data = r"D:\Programowanie\OpenMorrowind\data\Data Files"
    out = os.path.join(here, "..", "romfs")
    esm = M.ESM(); esm.load(os.path.join(data, "Morrowind.esm"))
    # the exterior cells baked (romfs/cells/X_Y.bin, X < 100), the interiors as mwbake moved them
    cells = []
    for fn in os.listdir(os.path.join(out, "cells")):
        x, y = map(int, fn[:-4].split("_"))
        if x < 100: cells.append((x, y))
    names = []
    with open(os.path.join(out, "interiors.bin"), "rb") as f:
        d = f.read()
    n = struct.unpack_from("<I", d, 4)[0]
    for k in range(n):
        gx, gy, fog, nm = struct.unpack_from("<iiI64s", d, 8 + k * 76)
        names.append((gx, gy, nm.split(b"\0")[0].decode("utf-8")))
    shifts = {}
    for gx, gy, name in names:
        it = esm.interiors.get(name)
        P = np.array([r["pos"][:3] for r in it["refs"]])
        shifts[name] = (gx * M.CELL + M.HALF - float((P[:, 0].min() + P[:, 0].max()) / 2),
                        gy * M.CELL + M.HALF - float((P[:, 1].min() + P[:, 1].max()) / 2), 1000.0 - float(P[:, 2].min()))
    bake(os.path.join(data, "Morrowind.esm"), out, cells, shifts)


if __name__ == "__main__":
    main()
