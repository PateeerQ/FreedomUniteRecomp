"""Makes the US build's seeds (seeds/us/) from the EU build's (seeds/).

    python tools/region_seeds.py EU_EBOOT US_EBOOT EU_DATA.BIN US_DATA.BIN

Seeds are function addresses the recompiler cannot find by itself
(scripts/setup.ps1): seeds/eboot.txt for the EBOOT, seeds/ov<hash>.txt for one
overlay. The US release is the EU one with a little more code near the end
of .text (host/region.h), so each address is looked up again in the US code:
the instructions around it (jump and branch targets and immediates masked
out) must match at exactly one place within 0x400 bytes. Overlays are paired
by their place in DATA.BIN (the same files in both), and their seeds moved by
the same search inside the US overlay (overlays load 0x100 higher in the US
build). An address that does not match is reported and left out.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "re"))
import databin  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_elf(p):
    d = open(p, "rb").read()
    phoff, = struct.unpack_from("<I", d, 0x1C)
    _, o, va, _, fs, ms, _, _ = struct.unpack_from("<8I", d, phoff)
    if va == 0:
        va = 0x08804000                               # PRX-style: loaded there
    mem = bytearray(ms)
    mem[:fs] = d[o:o + fs]
    return va, bytes(mem)


def mask(w):
    op = w >> 26
    if op in (2, 3):
        return w & 0xFC000000                         # j / jal: the target moves
    if op == 0:
        return w
    return w & 0xFFFF0000                             # immediates: offsets, lui halves


def window(mem, base, a, n=8):
    out = []
    for k in range(-n, n):
        x = a + 4 * k - base
        if x < 0 or x + 4 > len(mem):
            return None
        out.append(mask(struct.unpack_from("<I", mem, x)[0]))
    return out


def translate(a, src, dst, guess):
    """a in src (base, bytes) -> the one place in dst near a + guess, or None.
    Prologues and epilogues repeat, so a window that matches in several places
    is widened until one is left."""
    for n in (8, 32, 128):
        w = window(src[1], src[0], a, n)
        if w is None:
            return None
        hits = [a + guess + d for d in range(-0x400, 0x404, 4) if window(dst[1], dst[0], a + guess + d, n) == w]
        if len(hits) <= 1:
            return hits[0] if hits else None
    return None


def overlays(eboot, data):
    """DATA.BIN file index -> (hash, load address, bytes), the way databin does."""
    us = os.path.getsize(eboot) == databin.US_EBOOT_SIZE
    c = databin.Cipher(databin.eboot_bytes(eboot, databin.SBOX_ADDR + (0x120 if us else 0), 256))
    f = open(data, "rb")
    size = os.path.getsize(data)
    toc = struct.unpack("<%dI" % (databin.TOC_BYTES // 4), c.decrypt(f.read(databin.TOC_BYTES), 0))
    n = 0
    while n + 1 < len(toc) and toc[n + 1] > toc[n] and toc[n + 1] * databin.SECTOR <= size:
        n += 1
    out = {}
    for i in range(n):
        f.seek(toc[i] * databin.SECTOR)
        head = c.decrypt(f.read(64), toc[i])
        if head[:4] != b"MWo3":
            continue
        base, code, dsize = struct.unpack_from("<III", head, 8)
        size_all = 0x40 + code + dsize
        f.seek(toc[i] * databin.SECTOR)
        body = c.decrypt(f.read((size_all + 3) & ~3), toc[i])[:size_all]
        out[i] = (databin.fnv1a(body[:0x40 + code]), base, body)
    return out


def seeds(path):
    out = []
    for line in open(path, encoding="utf-8"):
        t = line.split("#")[0].strip()
        if t:
            out.append(int(t, 16))
    return out


def write(path, addrs, src_name):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(f"# Written by tools/region_seeds.py from seeds/{src_name}: the same functions in the US build.\n")
        for a in addrs:
            f.write(f"{a:08X}\n")


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    eu_eboot, us_eboot, eu_data, us_data = sys.argv[1:]
    EU, US = load_elf(eu_eboot), load_elf(us_eboot)
    bad = 0

    got = []
    for a in seeds(os.path.join(REPO, "seeds", "eboot.txt")):
        b = translate(a, EU, US, 0) or translate(a, EU, US, 0x120)
        if b is None:
            print(f"eboot: {a:08X}: no single match in the US EBOOT, left out")
            bad += 1
        else:
            got.append(b)
    write(os.path.join(REPO, "seeds", "us", "eboot.txt"), got, "eboot.txt")
    print(f"eboot: {len(got)} seed(s)")

    eu_ov, us_ov = overlays(eu_eboot, eu_data), overlays(us_eboot, us_data)
    by_hash = {h: i for i, (h, _, _) in eu_ov.items()}
    for name in sorted(os.listdir(os.path.join(REPO, "seeds"))):
        if not (name.startswith("ov") and name.endswith(".txt")):
            continue
        h = int(name[2:-4], 16)
        if h not in by_hash or by_hash[h] not in us_ov:
            print(f"{name}: no such overlay in the EU DATA.BIN (a mod's?), left out")
            continue
        i = by_hash[h]
        _, eb, ebody = eu_ov[i]
        uh, ub, ubody = us_ov[i]
        src, dst = (eb, ebody), (ub, ubody)
        got = []
        for a in seeds(os.path.join(REPO, "seeds", name)):
            b = translate(a, src, dst, ub - eb)
            if b is None:
                print(f"{name}: {a:08X}: no single match in the US overlay, left out")
                bad += 1
            else:
                got.append(b)
        write(os.path.join(REPO, "seeds", "us", f"ov{uh:08X}.txt"), got, name)
        print(f"{name} -> ov{uh:08X}.txt: {len(got)} seed(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
