"""Who in a save slot points into a few small blocks, and what the blocks hold.

Usage: blk_refs.py SLOT BASE+SIZE ...  (hex, e.g. 011B0000+2000)
"""
import collections, mmap, re, sys
import numpy as np

H = r"C:\Program Files (x86)\Steam\steamapps\common\Haydee"
slot = sys.argv[1]
want = [tuple(int(x, 16) for x in a.split("+")) for a in sys.argv[2:]]

mods, regs = [], []
for ln in open(f"{H}\\d3d9sw_slot{slot}.regions"):
    p = ln.split()
    if ln.startswith("# module"):
        mods.append((int(p[2], 16), int(p[3], 16), p[4]))
    elif not ln.startswith("#") and len(p) == 4:
        regs.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))

f = open(f"{H}\\d3d9sw_slot{slot}.bin", "rb")
mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

def where(a):
    for lo, hi, n in mods:
        if lo <= a < hi:
            return f"{n}+{a - lo:X}"
    for b, s, o in regs:
        if b <= a < b + s:
            return f"region {b:08X}"
    return "?"

for wb, ws in want:
    hits = collections.Counter()
    ex = []
    for b, s, o in regs:
        if wb <= b < wb + ws:
            continue
        n = (s // 4) * 4
        for c in range(0, n, 1 << 24):
            m = min(1 << 24, n - c)
            w = np.frombuffer(mm, dtype=np.uint32, count=m // 4, offset=o + c)
            for i in np.nonzero((w >= wb) & (w < wb + ws))[0]:
                at = b + c + 4 * int(i)
                k = where(at)
                hits[re.sub(r"\+[0-9A-F]+$", "", k)] += 1
                if len(ex) < 6:
                    ex.append(f"{k} -> {int(w[i]):08X}")
    body = next((mm[o + wb - b:o + wb - b + min(ws, 256)] for b, s, o in regs
                 if b <= wb < b + s), b"")
    txt = re.findall(rb"[ -~]{6,}|(?:[ -~]\x00){6,}", bytes(body))
    print(f"{wb:08X}+{ws:X}: {sum(hits.values())} reference(s) {dict(hits.most_common(8))}")
    for e in ex:
        print(f"    {e}")
    print(f"    first bytes: {bytes(body[:32]).hex(' ')}")
    strs = [t.replace(b"\x00", b"").decode()[:40] for t in txt[:5]]
    print(f"    text: {strs}")
