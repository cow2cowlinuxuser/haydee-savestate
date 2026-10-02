"""Where the game keeps pointers to its PhysX objects, read from a save slot.

Takes the objects physx_journal.csv saw (every 'self' and every returned new
object), checks each still carries its PhysX vtable in d3d9sw_slot0.bin (so the
slot and journal are one session), then finds every aligned word in the saved
memory equal to an object's address and says what kind of memory holds it.
"""
import collections, csv, mmap, struct, sys
import numpy as np

H = r"C:\Program Files (x86)\Steam\steamapps\common\Haydee"
slot = sys.argv[1] if len(sys.argv) > 1 else "0"

mods, regs, bands = [], [], []
for ln in open(f"{H}\\d3d9sw_slot{slot}.regions"):
    p = ln.split()
    if ln.startswith("# module"):
        mods.append((int(p[2], 16), int(p[3], 16), p[4]))
    elif ln.startswith("# band"):
        bands.append((int(p[2], 16), int(p[3], 16), p[4]))
    elif not ln.startswith("#") and len(p) == 4:
        regs.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))

def mod_of(a):
    for lo, hi, n in mods:
        if lo <= a < hi:
            return n, lo
    return None, 0

px = next(lo for lo, hi, n in mods if n.lower() == "physx3_x86.dll")

rows = list(csv.DictReader(open(f"{H}\\physx_journal.csv")))
objs = {}
for r in rows:
    t = r["table"]
    rva = int(t.split("_")[-1] if t.startswith("NpScene") else t[2:], 16)
    objs.setdefault(int(r["self"], 16), (t, rva))
names = {"vt187844": "PxPhysics", "NpScene_188C84": "scene", "vt188100": "body",
         "vt189E3C": "shape", "vt186A68": "constraint", "vt186F58": "material"}

f = open(f"{H}\\d3d9sw_slot{slot}.bin", "rb")
mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

def word(a):
    for b, s, o in regs:
        if b <= a < b + s - 3:
            return struct.unpack_from("<I", mm, o + a - b)[0]
    return None

ok = bad = gone = 0
for a, (t, rva) in objs.items():
    w = word(a)
    if w is None:
        gone += 1
    elif w - px == rva:
        ok += 1
    else:
        bad += 1
print(f"{len(objs)} objects: {ok} still carry their vtable, {bad} do not, {gone} not in the slot")

objs.pop(0, None)
want = set(objs)
keys = np.array(sorted(want), dtype=np.uint32)
refs = collections.defaultdict(list)
for b, s, o in regs:
    n = (s // 4) * 4
    if b % 4 or n == 0:
        continue
    for c in range(0, n, 1 << 24):
        m = min(1 << 24, n - c)
        w = np.frombuffer(mm, dtype=np.uint32, count=m // 4, offset=o + c)
        j = np.minimum(np.searchsorted(keys, w), len(keys) - 1)
        for i in np.nonzero(keys[j] == w)[0]:
            refs[int(w[i])].append(b + c + 4 * int(i))
    if s > 1 << 26:
        print(f"  scanned {b:08X} +{s >> 20} MB, {sum(map(len, refs.values()))} hits so far")
print("scan done")

def where(a):
    n, lo = mod_of(a)
    if n:
        return f"module {n}"
    for lo, hi, k in bands:
        if lo <= a < hi:
            return f"band {k}"
    k = int(np.searchsorted(keys, a, side="right")) - 1
    if k >= 0 and a - int(keys[k]) < 0x400:
        return "inside a PhysX object"
    return "other memory"

per = collections.defaultdict(collections.Counter)
for v, at in refs.items():
    kind = names.get(objs[v][0], objs[v][0])
    for x in at:
        per[kind][where(x)] += 1
print("\nreferences to each kind of object, by where the referring word lives:")
for k, c in sorted(per.items()):
    print(f"  {k:12} {sum(c.values()):6}  {dict(c.most_common())}")
unref = collections.Counter(names.get(objs[a][0], objs[a][0]) for a in objs if a not in refs)
print("\nobjects nothing in the slot points at:", dict(unref))

print("\nbands:", [(f"{lo:08X}", f"{hi:08X}", k) for lo, hi, k in bands])
for a in sorted(objs)[:3] + sorted(objs)[-2:]:
    print(f"  object {a:08X} lies in: {where(a)}; region",
          next((f"{b:08X}+{s >> 10}K" for b, s, o in regs if b <= a < b + s), "none"))

print("\nreferences from outside PhysX objects, grouped by kind and by 64K block:")
outside = collections.defaultdict(collections.Counter)
for v, at in refs.items():
    kind = names.get(objs[v][0], objs[v][0])
    for x in at:
        w = where(x)
        if w != "inside a PhysX object":
            outside[kind][f"{w} {x & ~0xFFFF:08X}"] += 1
for k, c in sorted(outside.items()):
    print(f"  {k:12} {dict(c.most_common(6))}")

code = [(lo, hi, n) for lo, hi, n in mods
        if n.lower() in ("haydee.dll", "physx3_x86.dll", "physx3common_x86.dll")]

def vtab_mod(v):
    for lo, hi, n in code:
        if lo <= v < hi:
            t = word(v)
            if t is not None and lo <= t < hi:
                return n, v - lo
    return None

def owner(x):
    for back in range(0, 0x400, 4):
        w = word(x - back)
        if w is None:
            return "?", 0, back
        m = vtab_mod(w)
        if m:
            return m[0], m[1], back
    return "none within 1K", 0, 0

print("\nwho holds each reference (nearest vtable at or before the word):")
held = collections.defaultdict(collections.Counter)
game = collections.Counter()
for v, at in refs.items():
    kind = names.get(objs[v][0], objs[v][0])
    for x in at:
        n, rva, back = owner(x)
        held[kind][n] += 1
        if n.lower() == "haydee.dll":
            game[(kind, f"vt{rva:06X}", back)] += 1
for k, c in sorted(held.items()):
    print(f"  {k:12} {dict(c)}")
print("\ngame-owned holders: (object kind, holder vtable rva in haydee.dll, field offset): count")
for (k, vt, off), n in sorted(game.items()):
    print(f"  {k:12} {vt} +{off:03X}  x{n}")
