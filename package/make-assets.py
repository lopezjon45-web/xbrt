#!/usr/bin/env python3
"""Writes the flat PNG tiles a package manifest names, one solid colour, no image library needed.

    make-assets.py <out dir>
"""
import struct
import sys
import zlib
from pathlib import Path

TILES = {"StoreLogo.png": (50, 50), "Square150x150Logo.png": (150, 150), "Square44x44Logo.png": (44, 44),
         "SplashScreen.png": (620, 300), "Wide310x150Logo.png": (310, 150)}
COLOUR = (32, 96, 160, 255)


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)


def png(width, height):
    row = b"\x00" + bytes(COLOUR) * width
    raw = row * height
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, (w, h) in TILES.items():
        (out / name).write_bytes(png(w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
