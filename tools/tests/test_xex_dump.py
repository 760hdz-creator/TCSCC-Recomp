"""xex_dump on Conviction's disc default.xex: flat image matches the recorded hash, header is written."""
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "tools/xex_dump/xex_dump.exe"
XEX = ROOT / "assets/default.xex"


def run(*args):
    return subprocess.run([str(EXE), *map(str, args)], capture_output=True, text=True)


@unittest.skipUnless(XEX.exists() and EXE.exists(), "needs assets/default.xex + built xex_dump")
class XexDump(unittest.TestCase):
    def test_image_matches_expected_hash(self):
        # Paths with spaces and parentheses (the repo lives in "G:\Project Zero").
        with tempfile.TemporaryDirectory(prefix="pz (img) ") as d:
            img = Path(d) / "default.img"
            r = run(XEX, "--out-img", img)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(img.stat().st_size, 0x18C0000)
            want = (ROOT / "config/expected_image.txt").read_text().split()[0]
            self.assertEqual(hashlib.sha256(img.read_bytes()).hexdigest(), want)

    def test_out_header_writes_xex_header(self):
        with tempfile.TemporaryDirectory() as d:
            hdr = Path(d) / "default.xexhdr"
            r = run(XEX, "--out-header", hdr)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(hdr.read_bytes()[:4], b"XEX2")

    def test_unwritable_outputs_fail(self):
        r = run(XEX, "--out-img", ROOT / "no_such_dir" / "x.img")
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
