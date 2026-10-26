"""make-disc.py <lab output folder>: writes media/ramp.bin, 200000 bytes where byte i is (i * 7 + (i >> 8)) & 0xFF."""
import pathlib
import sys

out = pathlib.Path(sys.argv[1]) / "media" / "ramp.bin"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(bytes((i * 7 + (i >> 8)) & 0xFF for i in range(200000)))
