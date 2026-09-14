"""Synthetic 3840x2160 bench frame carrying a valid JustFlow addon UI-mask strip (3 rects).
Stdlib only. Encoding mirrors addon\\JustFlow\\JustFlow.lua: 4x4 px cells at the top-left,
cell 0 = header (0x4A, 0x46, count), two cells per rect (x0,y0,x1,y1 as 12-bit packed into 6
bytes), last cell = checksum (sum of rect bytes & 255) in R=G=B.

    python tools\\make_strip_png.py [out.png]      (default build\\strip4k.png)
Then: build\\justflow.exe --ini profiles\\wow.ini --bench build\\strip4k.png --frames 10 --no-present
and look for "[ui] addon mask: 3 rects, checksum ok" in build\\bench.log.
"""
import os, struct, sys, zlib

W, H, CELL = 3840, 2160, 4
RECTS = [(100, 1900, 1500, 2100), (3300, 50, 3800, 500), (1600, 1000, 2200, 1300)]


def strip_cells(rects):
    cells = [(0x4A, 0x46, len(rects))]
    total = 0
    for x0, y0, x1, y1 in rects:
        b = (x0 >> 4, ((x0 & 15) << 4) | (y0 >> 8), y0 & 255, x1 >> 4, ((x1 & 15) << 4) | (y1 >> 8), y1 & 255)
        total += sum(b)
        cells += [b[:3], b[3:]]
    c = total & 255
    cells.append((c, c, c))
    return cells


def png(path, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "build", "strip4k.png")
    cells = strip_cells(RECTS)
    base = bytearray(W * 3)   # R: x gradient, B: 128; G set per row
    for x in range(W):
        base[x * 3] = x * 255 // (W - 1)
        base[x * 3 + 2] = 128
    rows = []
    for y in range(H):
        r = bytearray(base)
        r[1::3] = bytes([y * 255 // (H - 1)]) * W
        for x0, y0, x1, y1 in RECTS:   # visible fill so a human can spot the rects
            if y0 <= y < y1:
                r[x0 * 3:x1 * 3] = bytes([230, 230, 40]) * (x1 - x0)
        if y < CELL:
            for i, (cr, cg, cb) in enumerate(cells):
                r[i * CELL * 3:(i + 1) * CELL * 3] = bytes([cr, cg, cb]) * CELL
        rows.append(r)
    png(out, rows)
    print(f"wrote {out}: {len(RECTS)} rects {RECTS}, checksum {cells[-1][0]}")


if __name__ == "__main__":
    main()
