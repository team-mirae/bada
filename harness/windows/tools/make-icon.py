#!/usr/bin/env python3
"""Makes src/tip/bada.ico from the repository's logo (docs/logo/bada-icon-512.png).

Run on a Mac (it resizes with sips): tools/make-icon.py. The icon is kept in the repository, so a
build needs neither this script nor a Mac. Sizes 16, 32 and 48 are stored as bitmaps and 256 as a
PNG, which is what Windows' resource compiler and its icon loader both take.
"""
import pathlib
import struct
import subprocess
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
LOGO = HERE.parents[2] / "docs" / "logo" / "bada-icon-512.png"
OUT = HERE.parent / "src" / "tip" / "bada.ico"


def read_png(path):
    """A non-interlaced 8-bit RGBA or RGB PNG as (width, height, rows of RGBA bytes)."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    at, idat, width, height, kind = 8, b"", 0, 0, 0
    while at < len(data):
        length, tag = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if tag == b"IHDR":
            width, height, depth, kind, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and kind in (2, 6) and interlace == 0, (depth, kind, interlace)
        elif tag == b"IDAT":
            idat += body
        at += 12 + length
    channels = 4 if kind == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        mode, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if mode == 1:
                line[i] = (line[i] + left) & 255
            elif mode == 2:
                line[i] = (line[i] + up) & 255
            elif mode == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif mode == 4:
                p = left + up - corner
                a, b, c = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if a <= b and a <= c else up if b <= c else corner)) & 255
        previous = line
        if channels == 3:
            line = bytearray(b"".join(bytes(line[i:i + 3]) + b"\xff" for i in range(0, stride, 3)))
        rows.append(bytes(line))
    return width, height, rows


def bitmap(width, height, rows):
    """An icon's bitmap: a 32-bit DIB, bottom-up, BGRA, then an empty 1-bit mask."""
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(
        b"".join(bytes((row[i + 2], row[i + 1], row[i], row[i + 3])) for i in range(0, width * 4, 4))
        for row in reversed(rows))
    mask = bytes(((width + 31) // 32) * 4) * height
    return header + pixels + mask


def main():
    images = []
    with tempfile.TemporaryDirectory() as tmp:
        for size in (16, 32, 48, 256):
            small = pathlib.Path(tmp) / f"{size}.png"
            subprocess.run(["sips", "-z", str(size), str(size), str(LOGO), "--out", str(small)],
                           check=True, capture_output=True)
            images.append((size, small.read_bytes() if size == 256 else bitmap(*read_png(small))))
    offset = 6 + 16 * len(images)
    out = struct.pack("<HHH", 0, 1, len(images))
    for size, image in images:
        out += struct.pack("<BBBBHHII", size & 255, size & 255, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    OUT.write_bytes(out + b"".join(image for _, image in images))
    print(f"{OUT}: {len(images)} sizes, {offset} bytes")


if __name__ == "__main__":
    main()
