"""Hex/float/int view around guest addrs in a SCDA_DUMP_RAM dump. usage: ramdump_peek.py dump addr[,addr...] [before] [after]"""
import struct, sys
path = sys.argv[1]; addrs = [int(a, 16) for a in sys.argv[2].split(',')]
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40; after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x40
with open(path, 'rb') as f:
    while True:
        h = f.read(8)
        if len(h) < 8: break
        a0, size = struct.unpack('<II', h); pos = f.tell()
        for t in addrs:
            if a0 <= t - before and t + after <= a0 + size:
                f.seek(pos + t - before - a0); d = f.read(before + after)
                print(f'== {t:08X}')
                for i in range(0, len(d), 4):
                    u = struct.unpack('>I', d[i:i+4])[0]; fl = struct.unpack('>f', d[i:i+4])[0]; s = struct.unpack('>i', d[i:i+4])[0]
                    print(f'  {t - before + i:08X} {u:08X} {fl:14.6g} {s:11d} {"(rot %.1f deg)" % (s * 360 / 65536) if -70000 < s < 70000 and s else ""}')
        f.seek(pos + size)
