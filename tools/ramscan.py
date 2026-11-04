"""Scan a SCDA_DUMP_RAM dump for big-endian float/u32 patterns.
usage: ramscan.py dump.bin f:<float>,<float>... [tol]   or   x:<hexbytes>
Prints guest addresses of matches (floats compared with relative tolerance, default 1e-5)."""
import struct, sys
import numpy as np

def records(path):
    with open(path, 'rb') as f:
        while True:
            h = f.read(8)
            if len(h) < 8: return
            addr, size = struct.unpack('<II', h)
            yield addr, f.read(size)

def main():
    path, pat = sys.argv[1], sys.argv[2]
    tol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-5
    if pat.startswith('x:'):
        needle = bytes.fromhex(pat[2:])
        for addr, data in records(path):
            i = data.find(needle)
            while i >= 0:
                print(f'{addr + i:08X}'); i = data.find(needle, i + 1)
        return
    vals = [float(v) for v in pat[2:].split(',')]
    for addr, data in records(path):
        n = len(data) // 4
        for align in range(1):
            a = np.frombuffer(data[:n * 4], dtype='>f4').astype(np.float64)
            m = np.abs(a[:n - len(vals) + 1] - vals[0]) <= abs(vals[0]) * tol + 1e-6
            for k, v in enumerate(vals[1:], 1):
                m &= np.abs(a[k:n - len(vals) + 1 + k] - v) <= abs(v) * tol + 1e-6
            for i in np.nonzero(m)[0]:
                print(f'{addr + i * 4:08X}')

main()
