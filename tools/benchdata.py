#!/usr/bin/env python3
"""Reads the console's benchmark logs (benchlogs/*.log) into one JSON for docs/benchmarks.html.

Usage: python tools/benchdata.py [out.json]   (default docs/benchdata.json)
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "benchlogs")

# the tests in order: log file, short name, what changed
TESTS = [
    ("hw1-old3ds.log", "T1", "Pierwszy test: komórka wczytywana w głównej pętli, stały zasięg"),
    ("hw2-stream1-old3ds.log", "T2", "Streaming w wątku, przewidywanie ruchu, automatyczny zasięg (budżet GPU 16 ms), 30 FPS"),
    ("hw3-small1-old3ds.log", "T3", "Małe obiekty tylko blisko (37 % trójkątów), bez tekstu na dolnym ekranie w trakcie testu"),
    ("hw4-etc1-old3ds.log", "T4", "Tekstury ETC1 (3,6× mniej pamięci), teren 512² zamiast 256²"),
]

num = r"([-\d.]+)"


def parse(path):
    out = {"static": {}, "walks": [], "loads": []}
    cur_radius = None
    walk = None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip()
        m = re.match(r"radius (\d)[,:]", line)
        if m:
            cur_radius = int(m.group(1)); out["static"][cur_radius] = []; walk = None; continue
        m = re.match(r"\s+dir (\d): (?:fps " + num + r", )?cpu " + num + r" ms \(max " + num + r"\), gpu " + num +
                     r" ms, draws " + num + r", tris " + num, line)
        if m and cur_radius is not None:
            d = m.groups()
            out["static"][cur_radius].append({"dir": int(d[0]), "fps": float(d[1]) if d[1] else None, "cpu": float(d[2]),
                                              "gpu": float(d[4]), "draws": float(d[5]), "tris": float(d[6])})
            continue
        if line.startswith("walk") or line.startswith("sprint"):
            walk = {"name": "marsz" if "walk Seyda" in line else "sprint" if line.startswith("sprint") else "przejazd",
                    "seconds": [], "summary": None}
            out["walks"].append(walk); cur_radius = None; continue
        m = re.match(r"\s+(?:walk )?t=\s*" + num + r"s cell (-?\d+),(-?\d+): fps " + num + r", frame " + num +
                     r" ms \(max " + num + r"\), cpu " + num + r", gpu " + num + r"(?:, far " + num + r")?", line)
        if m and walk is not None:
            g = m.groups()
            walk["seconds"].append({"t": float(g[0]), "fps": float(g[3]), "frame": float(g[4]), "max": float(g[5]),
                                    "cpu": float(g[6]), "gpu": float(g[7]), "far": float(g[8]) if g[8] else None})
            continue
        m = re.match(r"\s+=> (\d+) frames, avg fps " + num + r", missed frames \(>34.5 ms\) (\d+), stops \(>50 ms\) (\d+), "
                     r"worst " + num + r" ms, loads (\d+) \(setup max " + num + r" ms\)", line)
        if m and walk is not None:
            g = m.groups()
            walk["summary"] = {"frames": int(g[0]), "fps": float(g[1]), "missed": int(g[2]), "stops": int(g[3]),
                               "worst": float(g[4]), "loads": int(g[5]), "setup": float(g[6])}
            continue
        m = re.match(r"\s+load (-?\d+),(-?\d+): (\d+) KB, read " + num + r" ms.*setup " + num, line)
        if m:
            out["loads"].append({"kb": int(m.group(3)), "read": float(m.group(4)), "setup": float(m.group(5))})
            continue
        m = re.match(r"\s+load: " + num + r" ms", line)
        if m:
            out["loads"].append({"kb": None, "read": None, "setup": float(m.group(1)), "sync": True})
    return out


def main():
    tests = []
    for fn, key, desc in TESTS:
        p = os.path.join(LOGS, fn)
        if not os.path.exists(p):
            continue
        d = parse(p); d["key"] = key; d["desc"] = desc
        tests.append(d)
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "docs", "benchdata.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(tests, f, ensure_ascii=False)
    # the report page: the template with the data inlined
    docs = os.path.join(HERE, "..", "docs")
    tpl = os.path.join(docs, "benchmarks.template.html")
    if os.path.exists(tpl):
        page = open(tpl, encoding="utf-8").read().replace("/*DATA*/", json.dumps(tests, ensure_ascii=False).replace("</", "<\\/"))
        with open(os.path.join(docs, "benchmarks.html"), "w", encoding="utf-8") as f:
            f.write(page)


if __name__ == "__main__":
    main()
