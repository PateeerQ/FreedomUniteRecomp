"""Decrypt MHFU's DATA.BIN (ULES01213, ULUS10391, or MHP2G's ULJM05500), the way the game does it.

    python tools/re/databin.py EBOOT DATA.BIN toc
    python tools/re/databin.py EBOOT DATA.BIN overlays OUT_DIR

The game's reader (0x0884EA1C) runs each byte through a 256-byte table in the
EBOOT (0x0899A018), then XORs each word with a key from two LCGs (0x0884ECAC:
k0 = k0 * 32653 % 0xFFF1, k1 = k1 * 9029 % 0xFFD9, key = k1 << 16 | k0). The
LCGs are seeded (0x0884EC3C) with the file's first sector; a zero half
becomes 32653 / 9029. The table of contents is the first 26632 bytes, seeded
with 0: word i is file i's first sector (2048-byte sectors).

`overlays` writes every file that is an MWo3 overlay as slot1_<hash>.bin-style
files (hash as hle_mhfu.c computes it: FNV-1a over header + code), named by
the slot its load address falls in. Nothing here is game data; the table is
read from the EBOOT you give it. The US EBOOT (told by its size) has the
table 0x120 higher and loads overlays 0x100 higher; the rest is the same."""
import os
import struct
import sys

SBOX_ADDR = 0x0899A018
TOC_BYTES = 26632
SECTOR = 2048
SLOTS = {0x09A5F200: 1, 0x09C19000: 2, 0x09D1A180: 3, 0x09D63000: 4}   # load address -> slot
US_EBOOT_SIZE = 1910672                          # ULUS10391: S-box +0x120, slots +0x100
JP_EBOOT_SIZE = 1890112                          # ULJM05500 (Portable 2nd G): its own places
JP_SBOX_ADDR = 0x089970C8                        # the same 256 bytes, found in the JP EBOOT
JP_SLOTS = {0x09A5A580: 1, 0x09C14280: 2, 0x09D15100: 3, 0x09D5DF80: 4}   # its slot table, 0x089C6B40


FUC_LOADER_WORD = 0x27BDFFE0                     # addiu $sp, $sp, -0x20: FUComplete's loader where the S-box was


def region_of(eboot):
    """(S-box address, slots) for the EBOOT given, told by its size. The S-box
    address is None for FUComplete (the English MHP2G): its EBOOT is the JP one
    with a loader where the table was, and its DATA.BIN is not encrypted."""
    n = os.path.getsize(eboot)
    if n == US_EBOOT_SIZE:
        return SBOX_ADDR + 0x120, {a + 0x100: s for a, s in SLOTS.items()}
    if n == JP_EBOOT_SIZE:
        return JP_SBOX_ADDR, JP_SLOTS
    if struct.unpack("<I", eboot_bytes(eboot, JP_SBOX_ADDR, 4))[0] == FUC_LOADER_WORD:
        return None, JP_SLOTS
    return SBOX_ADDR, SLOTS


def eboot_bytes(path, addr, n):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF', 'EBOOT must be the decrypted ELF'
    phoff, = struct.unpack_from('<I', d, 28)
    phnum, = struct.unpack_from('<H', d, 44)
    for i in range(phnum):
        typ, off, vaddr, _, filesz = struct.unpack_from('<IIIII', d, phoff + 32 * i)
        # PRX-style EBOOTs are linked at 0; the game loads them at 0x08804000.
        for base in (0, 0x08804000):
            lo = vaddr + base
            if typ == 1 and lo <= addr and addr + n <= lo + filesz:
                return d[off + addr - lo: off + addr - lo + n]
    raise SystemExit('address %08X not in the EBOOT' % addr)


class Cipher:
    def __init__(self, sbox):
        self.sbox = bytes(sbox) if sbox is not None else None

    def decrypt(self, data, seed):
        if self.sbox is None:                    # FUComplete: stored as it is
            return bytes(data)
        k0, k1 = seed & 0xFFFF, seed >> 16 & 0xFFFF
        k0 = k0 or 32653
        k1 = k1 or 9029
        sub = bytes(data).translate(self.sbox)
        n = len(sub) // 4
        words = struct.unpack('<%dI' % n, sub[:n * 4])
        out = []
        for w in words:
            k0 = k0 * 32653 % 0xFFF1
            k1 = k1 * 9029 % 0xFFD9
            out.append(w ^ (k1 << 16 | k0))
        return struct.pack('<%dI' % n, *out) + sub[n * 4:]


def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    eboot, data, cmd = sys.argv[1:4]
    sbox, slots = region_of(eboot)
    c = Cipher(eboot_bytes(eboot, sbox, 256) if sbox is not None else None)
    f = open(data, 'rb')
    size = os.path.getsize(data)
    toc = struct.unpack('<%dI' % (TOC_BYTES // 4), c.decrypt(f.read(TOC_BYTES), 0))
    nfiles = 0
    while nfiles + 1 < len(toc) and toc[nfiles + 1] > toc[nfiles] and toc[nfiles + 1] * SECTOR <= size:
        nfiles += 1
    if cmd == 'toc':
        print('first words:', ' '.join('%X' % x for x in toc[:12]))
        print('%d files with increasing sectors' % nfiles)
        return
    if cmd != 'overlays':
        raise SystemExit(__doc__)
    out = sys.argv[4]
    os.makedirs(out, exist_ok=True)
    found = 0
    for i in range(nfiles):
        start = toc[i] * SECTOR
        f.seek(start)
        head = c.decrypt(f.read(64), toc[i])
        if head[:4] != b'MWo3':
            continue
        base, code, dsize = struct.unpack_from('<III', head, 8)
        n = 0x40 + code + dsize
        f.seek(start)
        body = c.decrypt(f.read((n + 3) & ~3), toc[i])[:n]
        name = body[32:64].split(b'\0')[0].decode('ascii', 'replace')
        h = fnv1a(body[:0x40 + code])
        slot = slots.get(base, 9)
        path = os.path.join(out, 'slot%d_%08X.bin' % (slot, h))
        open(path, 'wb').write(body)
        print('file %4d  %-20s %08X at %08X, %d bytes' % (i, name, h, base, n))
        found += 1
    print('%d overlays' % found)


if __name__ == '__main__':
    main()
