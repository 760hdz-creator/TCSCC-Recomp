"""Find absolute jump tables XenonAnalyse misses and write config/pz_switches_manual.toml.

Pattern (Conviction's compiler, index in rN, table via r12/r0):
    cmplwi crF, rN, max
    bgt    crF, default
    lis    r12, hi
    rlwinm r0, rN, 2, 0, 29
    addi   r12, r12, lo
    lwzx   r0, r12, r0
    mtctr  r0
    bctr
The preamble instructions may be interleaved/reordered; we search back up to 16 insns.
Tables already in config/pz_switches.toml (XenonAnalyse) are skipped.

usage: python -I tools/find_jump_tables.py
"""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMG = ROOT / "assets" / "default.img"
AUTO = ROOT / "config" / "pz_switches.toml"
OUT = ROOT / "config" / "pz_switches_manual.toml"
BASE = 0x82000000
CODE = (0x824B0000, 0x83590000)

LWZX_R0_R12_R0 = 0x7C0C002E
MTCTR_R0 = 0x7C0903A6
BCTR = 0x4E800420


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def find(img):
    words = struct.unpack(f">{len(img) // 4}I", img[: len(img) // 4 * 4])
    lo_i, hi_i = (CODE[0] - BASE) // 4, (CODE[1] - BASE) // 4
    tables, misses = [], []
    for i in range(lo_i + 3, hi_i):
        if words[i] != BCTR or words[i - 1] != MTCTR_R0 or words[i - 2] != LWZX_R0_R12_R0:
            continue
        addr = BASE + i * 4
        hi = lo = idx = cmp_max = default = None
        for k in range(i - 3, max(lo_i, i - 19), -1):
            w = words[k]
            op = w >> 26
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            if lo is None and op == 14 and rd == 12 and ra == 12:            # addi r12,r12,lo
                lo = s16(w & 0xFFFF)
            elif hi is None and op == 15 and rd == 12 and ra == 0:           # lis r12,hi
                hi = w & 0xFFFF
            elif idx is None and op == 21 and ra == 0 and ((w >> 11) & 31) == 2 \
                    and ((w >> 6) & 31) == 0 and ((w >> 1) & 31) == 29:      # rlwinm r0,rN,2,0,29
                idx = rd  # rS field
            elif cmp_max is None and op == 10 and ((w >> 21) & 1) == 0:      # cmplwi crF,rN,imm
                if idx is None or ra == idx:
                    cmp_max, cmp_reg, crf = w & 0xFFFF, ra, (w >> 23) & 7
                    # bgt crF,target (bc BO=12 BI=crF*4+1) or bgtlr crF, anywhere before the bctr.
                    # XenonRecomp ignores `default`; bgtlr records 0.
                    for j in range(k + 1, i):
                        nb = words[j]
                        if ((nb >> 21) & 31) != 12 or ((nb >> 16) & 31) != crf * 4 + 1:
                            continue
                        if nb >> 26 == 16 and not nb & 3:
                            default = BASE + j * 4 + s16(nb & 0xFFFC)
                            break
                        if nb & 0xFC00FFFF == 0x4C000020:  # bclr
                            default = 0
                            break
            if None not in (hi, lo, idx, cmp_max, default):
                break
        if None not in (hi, lo, idx) and (cmp_max is None or cmp_reg != idx) \
                and ((hi << 16) + lo) & 0xFFFFFFFF == addr + 4:
            # Bounds check is far away (other block). Table is inline after the bctr:
            # take entries until code resumes (label must be near the bctr).
            off = i + 1
            labels = []
            while abs(words[off] - addr) < 0x10000 and CODE[0] <= words[off] < CODE[1]:
                labels.append(words[off])
                off += 1
            if labels:
                tables.append((addr, idx, 0, labels))
                continue
        if None in (hi, lo, idx, cmp_max, default) or cmp_reg != idx:
            misses.append(addr)
            continue
        table = ((hi << 16) + lo) & 0xFFFFFFFF
        off = (table - BASE) // 4
        labels = list(words[off: off + cmp_max + 1])
        if not all(CODE[0] <= l < CODE[1] for l in labels):
            misses.append(addr)
            continue
        tables.append((addr, idx, default, labels))
    return tables, misses


MTCTR_R12 = 0x7D8903A6
ADD_R12_R12_R0 = 0x7D8C0214
NOP = 0x60000000


heuristic = []  # offset-table sites sized by extent, not by a bound check


def find_offset(img):
    """Offset tables: label = base2 + (table[idx] << s) (lbzx: u8, lhzx: u16 with idx*2; s = 0, or 2 if
    `rlwinm r0,r0,2,0,29` follows the load).
        cmplwi crF,rN,max ; bgt ; lis r12,hi1 ; [rlwinm r0,rN,1,0,30] ; addi r12,r12,lo1
        lbzx|lhzx r0,r12,(rN|r0) ; lis r12,hi2 ; addi r12,r12,lo2 ; add r12,r12,r0 ; mtctr r12 ; bctr
    nops may be interleaved."""
    words = struct.unpack(f">{len(img) // 4}I", img[: len(img) // 4 * 4])
    lo_i, hi_i = (CODE[0] - BASE) // 4, (CODE[1] - BASE) // 4
    tables, misses = [], []
    for i in range(lo_i + 8, hi_i):
        if words[i] != BCTR or words[i - 1] != MTCTR_R12:
            continue
        seq = [k for k in range(i - 2, max(lo_i, i - 24), -1) if words[k] != NOP]
        if not seq or words[seq[0]] != ADD_R12_R12_R0:
            continue
        addr = BASE + i * 4
        st = {}
        cmps, defs = [], {}  # cmplwi (reg, max); defs[reg] = ("mr", src) | ("addi", src, imm), nearest first
        for k in seq[1:]:
            w = words[k]
            op, rd, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            xo = (w >> 1) & 0x3FF
            if op == 14 and rd == 12 and ra == 12:
                st.setdefault("lo2" if "load" not in st else "lo1", s16(w & 0xFFFF))
            elif op == 15 and rd == 12 and ra == 0:
                st.setdefault("hi2" if "load" not in st else "hi1", w & 0xFFFF)
            elif op == 21 and rd == 0 and ra == 0 and rb == 2 and "load" not in st:          # rlwinm r0,r0,2 after the load
                st["shift"] = 2
            elif op == 31 and rd == 0 and ra == 12 and xo in (87, 279) and "load" not in st:  # lbzx / lhzx
                st["load"], st["ib"] = ("u8" if xo == 87 else "u16"), rb
            elif op == 21 and ra == 0 and rb == 1 and "load" in st and st["ib"] == 0 and "idx" not in st:     # rlwinm r0,rN,1,0,30
                st["idx"] = rd
            elif op == 10 and not (w >> 21) & 1 and "load" in st:                           # cmplwi
                cmps.append((ra, w & 0xFFFF))
            elif "load" in st and op == 31 and xo == 444 and rd == rb:                       # mr rA,rS
                defs.setdefault(ra, ("mr", rd))
            elif "load" in st and op == 14 and ra != 0:                                       # addi rD,rA,imm
                defs.setdefault(rd, ("addi", ra, w & 0xFFFF))
        reg = st.get("idx", st.get("ib"))

        def same(a, b):  # a and b hold the same value at the jump (copy or identical addi)
            return a == b or defs.get(a) == ("mr", b) or defs.get(b) == ("mr", a) \
                or (defs.get(a) is not None and defs.get(a) == defs.get(b) and defs[a][0] == "addi")

        for cr_reg, mx in cmps:
            if reg is not None and same(reg, cr_reg):
                st["max"] = mx
                break
        st["reg"] = reg
        if not {"lo1", "hi1", "lo2", "hi2", "load"} <= st.keys() or reg is None:
            misses.append(addr)
            continue
        t = ((st["hi1"] << 16) + st["lo1"]) & 0xFFFFFFFF
        b2 = ((st["hi2"] << 16) + st["lo2"]) & 0xFFFFFFFF
        o = t - BASE
        size = 1 if st["load"] == "u8" else 2

        def label(j):
            x = img[o + j] if size == 1 else struct.unpack(">H", img[o + 2 * j: o + 2 * j + 2])[0]
            return b2 + (x << st.get("shift", 0))

        if "max" in st:
            labels = [label(j) for j in range(st["max"] + 1)]
        else:
            # Bound check lives in another block: HEURISTIC extent, take entries while the label
            # stays aligned and near the jump (max 256). Extra entries are unused labels: harmless.
            labels = []
            while len(labels) < 256 and not label(len(labels)) & 3 and abs(label(len(labels)) - addr) < 0x4000:
                labels.append(label(len(labels)))
            heuristic.append(addr)
        if not labels or not all(CODE[0] <= l < CODE[1] and not l & 3 for l in labels):
            misses.append(addr)
            continue
        tables.append((addr, st["reg"], 0, labels))
    return tables, misses


def main():
    img = IMG.read_bytes()
    auto = AUTO.read_text().split("# ---- MANUAL (config/pz_switches_manual.toml) ----")[0]  # recomp.sh appends ours
    known = {int(a, 16) for a in re.findall(r"base = 0x([0-9A-F]+)", auto)}
    tables, misses = find(img)
    t2, m2 = find_offset(img)
    print(f"offset tables {len(t2)} (extent-sized {len(heuristic)}: {' '.join(f'{a:X}' for a in heuristic)}), unmatched offset sites {len(m2)}")
    tables += t2
    misses += m2
    # XenonAnalyse keys the same jump at an earlier instruction (<= 0x40 before the bctr): skip those.
    new = [t for t in tables if not any(0 <= t[0] - k < 0x40 for k in known)]
    out = ["# ---- MANUAL (config/pz_switches_manual.toml) ----",
           "# Generated by tools/find_jump_tables.py (absolute r12/r0 tables XenonAnalyse misses)."]
    for addr, r, default, labels in new:
        out += ["[[switch]]", f"base = 0x{addr:X}", f"r = {r}", f"default = 0x{default:X}", "labels = ["]
        out += [f"    0x{l:X}," for l in labels]
        out += ["]", ""]
    OUT.write_text("\n".join(out) + "\n")
    print(f"tables {len(tables)} (new {len(new)}, already known {len(tables) - len(new)}), unmatched bctr sites {len(misses)}")
    for m in misses[:20]:
        print(f"  unmatched 0x{m:X}")


if __name__ == "__main__":
    main()
