"""The LOD level baked by mwbake.py: a lighter copy of every model part, drawn from the middle distance on.

Two methods, because the measurements (docs/POMYSLY.md) showed one does not fit both kinds of parts:
  - solid parts (rocks, walls, trunks): quadric edge collapse (pyfqmr), to LOD_RATIO of the triangles, open
    borders kept (texture seams in NIF meshes are split vertices, so they are borders too). Rocks: 564 -> 196.
  - alpha-tested parts (leaves): hundreds of separate 2-triangle cards, which edge collapse cannot reduce
    (a tree: 1688 -> 1220). Instead a share of the cards is dropped and the kept ones are scaled up about their
    own centre, so the crown stays about as dense from afar ("card thinning").

The new vertices take texture coordinates, colours and normals from the nearest original vertex.
"""
import numpy as np

try:
    import pyfqmr
except ImportError:   # LOD then only thins leaf cards
    pyfqmr = None

LOD_RATIO = 0.35          # solid parts: share of triangles kept
CARD_KEEP = 0.4           # leaves: share of cards kept
MIN_TRIS = 24             # smaller parts are kept as they are


def _nearest(src, dst):
    """For each point of dst, the index of the nearest point of src (brute force: parts are small)."""
    out = np.empty(len(dst), np.int64)
    for i in range(0, len(dst), 512):
        d = ((dst[i:i + 512, None, :] - src[None, :, :]) ** 2).sum(axis=2)
        out[i:i + 512] = d.argmin(axis=1)
    return out


def _components(tri, nv):
    """Connected components of the triangles (sharing a vertex): label per triangle."""
    parent = np.arange(nv)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b, c in tri:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    roots = np.array([find(i) for i in range(nv)])
    return roots[tri[:, 0]]


def thin_cards(s):
    tri = s["tri"]
    comp = _components(tri, len(s["v"]))
    ids = np.unique(comp)
    if len(ids) < 12:
        return None
    # keep a fixed share, spread evenly over the cards (deterministic: the same tree looks the same everywhere)
    keep_ids = ids[(np.arange(len(ids)) * CARD_KEEP) % 1.0 < CARD_KEEP]
    keep = np.isin(comp, keep_ids)
    t = tri[keep]
    used = np.unique(t)
    remap = -np.ones(len(s["v"]), np.int64); remap[used] = np.arange(len(used))
    v = s["v"][used].copy()
    # scale every kept card about its centre so the same area stays covered
    scale = 1.0 / np.sqrt(CARD_KEEP)
    vcomp = np.empty(len(s["v"]), np.int64); vcomp[tri.ravel()] = np.repeat(comp, 3)
    vc = vcomp[used]
    for c in np.unique(vc):
        m = vc == c
        ctr = v[m].mean(axis=0)
        v[m] = ctr + (v[m] - ctr) * scale
    out = dict(s)
    out.update(v=v, tri=remap[t], uv=s["uv"][used], base=s["base"][used], a=s["a"][used],
               n=None if s["n"] is None else s["n"][used])
    return out


def decimate(s):
    if pyfqmr is None:
        return None
    n = len(s["tri"])
    f = pyfqmr.Simplify()
    f.setMesh(s["v"].astype(np.float64), s["tri"].astype(np.int32))
    f.simplify_mesh(target_count=max(8, int(n * LOD_RATIO)), aggressiveness=7, preserve_border=True, verbose=0)
    v2, t2, _ = f.getMesh()
    if len(t2) >= n * 0.9:
        return None
    near = _nearest(s["v"], v2)
    out = dict(s)
    out.update(v=v2, tri=t2.astype(np.int64), uv=s["uv"][near], base=s["base"][near], a=s["a"][near],
               n=None if s["n"] is None else s["n"][near])
    return out


def lod_shape(s):
    """The LOD copy of one model part, or the part itself when it cannot be made lighter."""
    if len(s["tri"]) < MIN_TRIS:
        return s
    r = thin_cards(s) if s["alpha"] else decimate(s)
    return r if r is not None else s
