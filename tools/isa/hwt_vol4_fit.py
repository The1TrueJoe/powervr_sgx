"""Fit hwt_vol4's map_* results: for each volume size, the index bit each
coordinate bit lands on, then the mixed points checked against that order.

    python3 -I tools/isa/hwt_vol4_fit.py hwt_vol4.out

Prints, per size, the index bits from bit 0 up (x0, y0, z0, ...) and "ok" or
the mixed points that don't fit."""
import re
import sys
from collections import defaultdict


def index(v):
    """the dword a texel came from: format 0x0d returns its bytes 2, 1, 0, 3"""
    return (v >> 16 & 0xFF) | (v >> 8 & 0xFF) << 8 | (v & 0xFF) << 16 | (v >> 24) << 24

LINE = re.compile(r"^map_(\d+)x(\d+)x(\d+)_(\d+)_(\d+)_(\d+)\s+0x([0-9a-f]{8})")


def main():
    got = defaultdict(dict)
    for ln in open(sys.argv[1]):
        m = LINE.match(ln.strip())
        if m:
            w, h, d, x, y, z = map(int, m.groups()[:6])
            got[(w, h, d)][(x, y, z)] = index(int(m.group(7), 16))
    for (w, h, d), pts in sorted(got.items()):
        base = pts.get((0, 0, 0), 0)
        bits = {}
        for axis, n, name in ((0, w, "x"), (1, h, "y"), (2, d, "z")):
            k = 0
            while (1 << k) < n:
                p = [0, 0, 0]
                p[axis] = 1 << k
                v = pts.get(tuple(p))
                if v is not None:
                    diff = v ^ base
                    bits[f"{name}{k}"] = diff.bit_length() - 1 if diff and not diff & (diff - 1) else f"?{diff:#x}"
                k += 1
        order = sorted((b, n) for n, b in bits.items() if isinstance(b, int))
        odd = {n: b for n, b in bits.items() if not isinstance(b, int)}

        def predict(x, y, z):
            i = base
            for n, b in bits.items():
                if not isinstance(b, int):
                    return None
                c = {"x": x, "y": y, "z": z}[n[0]]
                if c >> int(n[1:]) & 1:
                    i |= 1 << b
            return i

        bad = [(p, v, predict(*p)) for p, v in pts.items() if predict(*p) != v]
        print(f"{w}x{h}x{d}: base {base:#x}  " + " ".join(f"{n}@{b}" for b, n in order) +
              (f"  odd {odd}" if odd else "") +
              ("  ok" if not bad else "  MISFIT " + " ".join(
                  f"{p}={v:#x}/{q if q is None else hex(q)}" for p, v, q in bad)))


if __name__ == "__main__":
    main()
