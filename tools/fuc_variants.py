"""FUComplete's MODLOAD.BIN patches some overlays as the game loads them (a hook
into its own code, or words changed), some only when an option in its
CONFIG.BIN is on. Each patched overlay hashes differently from the DATA.BIN
copy, and one that was never recompiled is a dispatch miss (GitHub issue #4: the
title screen's Options). This runs MODLOAD's patch code (a few MIPS
instructions per overlay) for every overlay it knows and every setting of the
options it reads, and writes each distinct result as an overlay to recompile,
with the seeds it needs: MODLOAD's hook functions (seeds/fuc/eboot.txt) and
where they jump back into the overlay (seeds/fuc/ov<hash>.txt).

    python tools/fuc_variants.py work/fuc/fuc_prepared_EBOOT.BIN \
        work/fuc/root/disc/PSP_GAME/USRDIR/DATA.BIN work/fuc/overlays seeds/fuc
"""
import itertools
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 're'))
import databin  # noqa: E402

# MODLOAD.BIN starts with its size (4 bytes); the code after it loads at 0x08A35910.
MODLOAD = 0x08A35910
MODLOAD_END = 0x08A364DC
SWITCH = 0x08A35AE0          # lw $a0, file index; beq $a0, N, case ...
EXIT = 0x08A35EA4            # where every case ends
CONFIG = 0x08A35850          # CONFIG.BIN in memory: the option bytes the cases read
SLOT1 = 0x09A5A580           # where slot-1 overlays load (JP layout)


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Machine:
    """Just what MODLOAD's patch cases use."""

    def __init__(self, eboot_read, overlay, config):
        self.r = [0] * 32
        self.eboot_read = eboot_read
        self.ovl = overlay            # bytearray loaded at SLOT1
        self.config = config          # dict offset -> byte
        self.hooks = []               # (site, target) of every `j` written into the overlay

    def in_ovl(self, a, n):
        return SLOT1 <= a and a + n <= SLOT1 + len(self.ovl)

    def read(self, a, n):
        if self.in_ovl(a, n):
            return int.from_bytes(self.ovl[a - SLOT1:a - SLOT1 + n], 'little')
        if CONFIG <= a < CONFIG + 0x40:
            return self.config.get(a - CONFIG, 0)
        return int.from_bytes(self.eboot_read(a, n), 'little')

    def write(self, a, v, n):
        if not self.in_ovl(a, n):
            return                        # the EBOOT's own data: not part of the overlay
        v &= (1 << (8 * n)) - 1
        self.ovl[a - SLOT1:a - SLOT1 + n] = v.to_bytes(n, 'little')
        if n == 4 and v >> 26 == 2:
            self.hooks.append((a, (a & 0xF0000000) | (v & 0x03FFFFFF) << 2))

    def run(self, pc):
        steps = 0
        delay = None
        while pc != EXIT:
            steps += 1
            if steps > 10000:
                raise SystemExit('MODLOAD case did not end (pc %08X)' % pc)
            w = struct.unpack('<I', self.eboot_read(pc, 4))[0]
            op, rs, rt, rd = w >> 26, w >> 21 & 31, w >> 16 & 31, w >> 11 & 31
            imm, sh, fn = w & 0xFFFF, w >> 6 & 31, w & 63
            r = self.r
            nxt = pc + 4
            target = None
            if w == 0:
                pass
            elif op == 0 and fn == 0x02:          # srl
                r[rd] = r[rt] >> sh
            elif op == 0 and fn == 0x00:          # sll
                r[rd] = (r[rt] << sh) & 0xFFFFFFFF
            elif op == 0 and fn in (0x20, 0x21):  # add, addu
                r[rd] = (r[rs] + r[rt]) & 0xFFFFFFFF
            elif op == 0x0F:                      # lui
                r[rt] = imm << 16
            elif op in (0x08, 0x09):              # addi, addiu
                r[rt] = (r[rs] + sx16(imm)) & 0xFFFFFFFF
            elif op == 0x0D:                      # ori
                r[rt] = r[rs] | imm
            elif op == 0x23:                      # lw
                r[rt] = self.read((r[rs] + sx16(imm)) & 0xFFFFFFFF, 4)
            elif op == 0x20:                      # lb
                v = self.read((r[rs] + sx16(imm)) & 0xFFFFFFFF, 1)
                r[rt] = (v - 0x100 if v & 0x80 else v) & 0xFFFFFFFF
            elif op == 0x24:                      # lbu
                r[rt] = self.read((r[rs] + sx16(imm)) & 0xFFFFFFFF, 1)
            elif op == 0x2B:                      # sw
                self.write((r[rs] + sx16(imm)) & 0xFFFFFFFF, r[rt], 4)
            elif op == 0x29:                      # sh
                self.write((r[rs] + sx16(imm)) & 0xFFFFFFFF, r[rt], 2)
            elif op == 0x28:                      # sb
                self.write((r[rs] + sx16(imm)) & 0xFFFFFFFF, r[rt], 1)
            elif op in (0x04, 0x05):              # beq, bne
                if (r[rs] == r[rt]) == (op == 0x04):
                    target = pc + 4 + (sx16(imm) << 2)
            else:
                raise SystemExit('fuc_variants: no %08X at %08X' % (w, pc))
            r[0] = 0
            if delay is not None:
                pc, delay = delay, None
                continue
            if target is not None:
                delay = target
            pc = nxt
        return self


def cases(eboot_read):
    """{DATA.BIN file index: case start} from MODLOAD's switch."""
    out = {}
    pc = SWITCH
    for _ in range(64):
        w = struct.unpack('<I', eboot_read(pc, 4))[0]
        if w >> 26 == 0x0D and (w >> 21 & 31) == 0:            # ori $a1, $zero, N (in a delay slot)
            n = w & 0xFFFF
            beq = struct.unpack('<I', eboot_read(pc + 4, 4))[0]
            if beq >> 26 == 0x04:
                out[n] = pc + 8 + (sx16(beq & 0xFFFF) << 2)
        if w >> 26 == 0x04 and (w >> 21 & 31) == 0 and (w >> 16 & 31) == 0:   # b: the default
            break
        pc += 4
    return out


def config_bytes_read(eboot_read, start):
    """The CONFIG.BIN offsets a case tests (lb $x, N($v1) with $v1 = CONFIG)."""
    offs = set()
    pc = start
    while pc < EXIT:
        w = struct.unpack('<I', eboot_read(pc, 4))[0]
        if w >> 26 in (0x20, 0x24) and (w >> 21 & 31) == 3:
            offs.add(w & 0xFFFF)
        pc += 4
        if w >> 26 == 0x04 and (w >> 21 & 31) == 0 and (w >> 16 & 31) == 0 and (w & 0xFFFF) != 0:
            nxt = struct.unpack('<I', eboot_read(pc, 4))[0]
            if nxt == 0 and pc + 4 - 4 + (sx16(w & 0xFFFF) << 2) == EXIT:
                break
    return sorted(offs)


def returns(eboot_read):
    """Every `j` from MODLOAD's code back into slot 1: where hooks return."""
    out = []
    for pc in range(MODLOAD, MODLOAD_END, 4):
        w = struct.unpack('<I', eboot_read(pc, 4))[0]
        if w >> 26 == 2:
            t = (pc & 0xF0000000) | (w & 0x03FFFFFF) << 2
            if 0x09A00000 <= t < 0x09E00000:
                out.append(t)
    return out


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    eboot, data, out_dir, seed_dir = sys.argv[1:5]
    img = open(eboot, 'rb').read()

    def eboot_read(a, n):
        return databin.eboot_bytes(eboot, a, n)

    # DATA.BIN's overlays, by file index
    sbox, slots = databin.region_of(eboot)
    c = databin.Cipher(databin.eboot_bytes(eboot, sbox, 256) if sbox is not None else None)
    f = open(data, 'rb')
    toc = struct.unpack('<%dI' % (databin.TOC_BYTES // 4), c.decrypt(f.read(databin.TOC_BYTES), 0))

    def overlay(i):
        start = toc[i] * databin.SECTOR
        f.seek(start)
        head = c.decrypt(f.read(64), toc[i])
        if head[:4] != b'MWo3':
            return None
        base, code, dsize = struct.unpack_from('<III', head, 8)
        n = 0x40 + code + dsize
        f.seek(start)
        return base, code, bytearray(c.decrypt(f.read((n + 3) & ~3), toc[i])[:n])

    back = returns(eboot_read)
    eboot_seeds = set()
    written = 0
    for idx, start in sorted(cases(eboot_read).items()):
        ov = overlay(idx)
        if not ov or ov[0] != SLOT1:
            continue                      # not a slot-1 overlay (file 15 is data)
        base, code, body = ov
        name = body[32:64].split(b'\0')[0].decode('ascii', 'replace')
        opts = config_bytes_read(eboot_read, start)
        # The seeds this overlay already has (its DATA.BIN copy's, the run-time copy's
        # found before): every variant needs them too (issue #5: game_task with both
        # options on missed 0x09B63C00, a seed of the default variant only).
        hashes = {databin.fnv1a(bytes(body[:0x40 + code]))}
        for bits in itertools.product((0, 1), repeat=len(opts)):
            m = Machine(eboot_read, bytearray(body), dict(zip(opts, bits))).run(start)
            hashes.add(databin.fnv1a(bytes(m.ovl[:0x40 + code])))
        inherited = set()
        for h0 in hashes:
            sp = os.path.join(seed_dir, 'ov%08X.txt' % h0)
            if os.path.exists(sp):
                inherited |= {int(l.split()[0], 16) for l in open(sp) if l.strip() and not l.startswith('#')}
        seen = set()
        for bits in itertools.product((0, 1), repeat=len(opts)):
            m = Machine(eboot_read, bytearray(body), dict(zip(opts, bits))).run(start)
            h = databin.fnv1a(bytes(m.ovl[:0x40 + code]))
            if h in seen:
                continue
            seen.add(h)
            path = os.path.join(out_dir, 'slot1_%08X.bin' % h)
            if not os.path.exists(path):
                open(path, 'wb').write(m.ovl)
                written += 1
            seeds = sorted({t for s, t in m.hooks for t in back if s < t <= s + 0x100 and m.in_ovl(t, 4)}
                           | {t for t in inherited if m.in_ovl(t, 4)})
            for s, t in m.hooks:
                if MODLOAD <= t < MODLOAD_END:
                    eboot_seeds.add(t)
            if seeds:
                sp = os.path.join(seed_dir, 'ov%08X.txt' % h)
                have = set()
                if os.path.exists(sp):
                    have = {l.split()[0].upper() for l in open(sp) if l.strip() and not l.startswith('#')}
                new = [s for s in seeds if '%08X' % s not in have]
                if new:
                    with open(sp, 'a') as sf:
                        if not have:
                            sf.write('# tools/fuc_variants.py: %s seeds (its other copies\', and where MODLOAD\'s '
                                     'hooks return)\n' % name)
                        for s in new:
                            sf.write('%08X\n' % s)
            print('file %d %-16s options %s = %s -> %08X, %d hooks' % (
                idx, name, ','.join('0x%X' % o for o in opts) or '-', bits, h, len(m.hooks)))
    ep = os.path.join(seed_dir, 'eboot.txt')
    have = {l.split()[0].upper() for l in open(ep) if l.strip() and not l.startswith('#')}
    new = sorted(s for s in eboot_seeds if '%08X' % s not in have)
    if new:
        with open(ep, 'a') as sf:
            sf.write('# tools/fuc_variants.py: MODLOAD\'s hook functions\n')
            for s in new:
                sf.write('%08X\n' % s)
    print('%d new overlays, %d new EBOOT seeds' % (written, len(new)))


if __name__ == '__main__':
    main()
