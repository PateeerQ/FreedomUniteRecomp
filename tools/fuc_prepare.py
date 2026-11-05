"""Make a recompilable EBOOT from Monster Hunter Portable 2nd G FUComplete (the
English fan translation of ULJM05500).

    python tools/fuc_prepare.py ISO OUT_EBOOT [CODE_PATCHES]

FUComplete's EBOOT (PSP_GAME/SYSDIR/EBOOT.BIN) is the Japanese one, already
decrypted, with a loader where the DATA.BIN cipher table was (0x089970C8; its
DATA.BIN is stored plain). At boot the loader reads FUC/PRELOAD.BIN to
0x089C7530 and runs it. PRELOAD copies its files to ms0:/PSP/SAVEDATA/FUCDAT,
loads FUC/MODLOAD.BIN to 0x08A35910, and then writes into the game's code:
`j` hooks into MODLOAD (0x0884E158, 0x0884E730, 0x0886CFE8) and many
immediates (text positions, widths, options from CONFIG.BIN).

A recompiled game cannot run code that only appears in memory at run time,
nor see writes into its own code. So:
  - PRELOAD and MODLOAD go into the EBOOT image at the addresses they are
    loaded to (in .bss), and are recompiled with it (seeds/fuc/eboot.txt);
  - CODE_PATCHES (seeds/fuc/codepatch.txt: "address word" lines) are the
    words PRELOAD leaves in the EBOOT's code, taken from a memory dump after
    boot (tools/fuc_prepare.py --diff EBOOT DUMP): they are written into the
    image too, so the recompiled code is the patched code.
Nothing written here is distributed: the output goes to work/fuc (gitignored).
"""
import struct
import sys

PRELOAD_AT = 0x089C7530
MODLOAD_AT = 0x08A35910
IMAGE_BASE = 0x08804000          # the EBOOT is linked at 0 and loaded here
TEXT_END = 0x0890B8E0            # the JP .text ends here (the code PRELOAD patches)


def iso_files(path):
    f = open(path, "rb")
    f.seek(16 * 2048)
    pvd = f.read(2048)
    out = {}

    def walk(lba, size, prefix):
        f.seek(lba * 2048)
        data = f.read(size)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + n]
            elba, esize = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            name = rec[33:33 + rec[32]]
            i += n
            if name in (b"\0", b"\1"):
                continue
            p = prefix + "/" + name.decode("ascii", "replace").split(";")[0]
            if rec[25] & 2:
                walk(elba, esize, p)
            else:
                out[p.upper()] = (elba, esize)

    root = pvd[156:190]
    walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")

    def read(p):
        lba, n = out[p.upper()]
        f.seek(lba * 2048)
        return f.read(n)
    return read


def load_ranges(d):
    """The ELF's first PT_LOAD: (file offset, vaddr, filesz, memsz, phdr offset)."""
    phoff, = struct.unpack_from("<I", d, 0x1C)
    phnum, = struct.unpack_from("<H", d, 0x2C)
    for i in range(phnum):
        at = phoff + 32 * i
        typ, off, va, pa, fs, ms = struct.unpack_from("<6I", d, at)
        if typ == 1:
            return off, va, fs, ms, at
    raise SystemExit("no PT_LOAD in the EBOOT")


def build(eboot, blobs):
    """`eboot` with its first segment's file part grown to hold `blobs`
    ({address: bytes}), everything after that segment moved along."""
    d = bytearray(eboot)
    off, va, fs, ms, ph = load_ranges(d)
    base = IMAGE_BASE if va == 0 else va
    end = max(a + len(b) for a, b in blobs.items()) - base
    end = (end + 15) & ~15
    if end > ms:
        raise SystemExit("the blobs go past the segment's memory")
    grow = max(0, end - fs)
    seg = bytearray(d[off:off + fs]) + bytes(grow)
    for a, b in blobs.items():
        seg[a - base:a - base + len(b)] = b
    old_end = off + fs
    out = d[:off] + seg + d[old_end:]
    # everything after the segment's old end moved by `grow`
    shoff, = struct.unpack_from("<I", out, 0x20)
    if shoff >= old_end:
        struct.pack_into("<I", out, 0x20, shoff + grow)
        shoff += grow
    phoff, = struct.unpack_from("<I", out, 0x1C)
    phnum, = struct.unpack_from("<H", out, 0x2C)
    for i in range(phnum):
        at = phoff + 32 * i
        o, = struct.unpack_from("<I", out, at + 4)
        if at == ph:
            struct.pack_into("<I", out, at + 16, fs + grow)
        elif o >= old_end:
            struct.pack_into("<I", out, at + 4, o + grow)
    shnum, = struct.unpack_from("<H", out, 0x30)
    for i in range(shnum if shoff else 0):
        at = shoff + 40 * i
        typ, = struct.unpack_from("<I", out, at + 4)
        o, = struct.unpack_from("<I", out, at + 16)
        if typ != 8 and o >= old_end:                    # not NOBITS
            struct.pack_into("<I", out, at + 16, o + grow)
    return bytes(out)


def code_patches(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        t = line.split("#")[0].split()
        if len(t) >= 2:
            out[int(t[0], 16)] = struct.pack("<I", int(t[1], 16))
    return out


def diff(eboot_path, dump_path):
    """The words in the EBOOT's code that differ in a memory dump (taken after
    PRELOAD ran): what to put in seeds/fuc/codepatch.txt."""
    d = open(eboot_path, "rb").read()
    off, va, fs, ms, ph = load_ranges(d)
    base = IMAGE_BASE if va == 0 else va
    mem = open(dump_path, "rb").read()
    print("# Written by tools/fuc_prepare.py --diff: the words PRELOAD.BIN leaves in the EBOOT's code")
    print("# (FUComplete's run-time patches, with its default CONFIG.BIN).")
    for a in range(base, TEXT_END, 4):
        e = d[off + a - base:off + a - base + 4]
        m = mem[a - 0x08800000:a - 0x08800000 + 4]
        if e != m:
            print(f"{a:08X} {struct.unpack('<I', m)[0]:08X}   # was {struct.unpack('<I', e)[0]:08X}")


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "--diff":
        return diff(sys.argv[2], sys.argv[3])
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    read = iso_files(sys.argv[1])
    eboot = read("/PSP_GAME/SYSDIR/EBOOT.BIN")
    if eboot[:4] != b"\x7fELF":
        raise SystemExit("PSP_GAME/SYSDIR/EBOOT.BIN is not a plain ELF: not a FUComplete disc?")
    pre, mod = read("/FUC/PRELOAD.BIN"), read("/FUC/MODLOAD.BIN")
    n_pre, n_mod = struct.unpack_from("<I", pre)[0], struct.unpack_from("<I", mod)[0]
    blobs = {PRELOAD_AT: pre[4:4 + n_pre], MODLOAD_AT: mod[4:4 + n_mod]}
    patches = code_patches(sys.argv[3]) if len(sys.argv) > 3 else {}
    out = build(eboot, blobs)
    if patches:
        out = bytearray(out)
        off, va, fs, ms, ph = load_ranges(out)
        base = IMAGE_BASE if va == 0 else va
        for a, w in patches.items():
            out[off + a - base:off + a - base + 4] = w
        out = bytes(out)
    open(sys.argv[2], "wb").write(out)
    print(f"fuc: PRELOAD {n_pre} bytes at {PRELOAD_AT:08X}, MODLOAD {n_mod} bytes at {MODLOAD_AT:08X}, "
          f"{len(patches)} code word(s) from run time; {len(out)} bytes")


if __name__ == "__main__":
    main()
