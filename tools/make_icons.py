# Renders JustFlow's icons: python tools/make_icons.py  (no dependencies; writes art/*.ico + art/icons_preview.png)
#
# The mark: a rounded badge with two forward chevrons - the solid one is the real frame, the translucent one
# behind it the generated in-between frame. Drawn from signed distance functions at each size (no scaling of
# a master image), so 16 px stays crisp: strokes get relatively thicker as the icon gets smaller.
#   justflow.ico     the NVIDIA build (green-teal)
#   justflow_xe.ico  the Intel/any-GPU build (blue-cyan) - also its tray icon while generating
#   xe_watching.ico  tray: watching a window, not generating (slate)
#   xe_off.ico       tray: automatic mode off (dim)
import math, os, struct, zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, '..', 'art')

VARIANTS = {
    #               badge gradient (top-left, bottom-right)   chevron rgb      lead, ghost opacity
    'justflow':    ((0x16, 0xA3, 0x4A), (0x0D, 0x94, 0x88), (255, 255, 255), 1.00, 0.45),
    'justflow_xe': ((0x25, 0x63, 0xEB), (0x06, 0xB6, 0xD4), (255, 255, 255), 1.00, 0.45),
    'xe_watching': ((0x47, 0x55, 0x69), (0x64, 0x74, 0x8B), (235, 240, 245), 0.95, 0.40),
    'xe_off':      ((0x3F, 0x48, 0x56), (0x4A, 0x54, 0x64), (205, 210, 218), 0.70, 0.30),   # dim, but findable on a dark taskbar
}


def sd_round_box(px, py, cx, cy, hx, hy, r):
    qx, qy = abs(px - cx) - hx + r, abs(py - cy) - hy + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sd_segment(px, py, ax, ay, bx, by):
    pax, pay, bax, bay = px - ax, py - ay, bx - ax, by - ay
    h = max(0.0, min(1.0, (pax * bax + pay * bay) / (bax * bax + bay * bay)))
    return math.hypot(pax - bax * h, pay - bay * h)


def sd_chevron(px, py, x0, half_h, depth, cy, thick):
    # ">" with its tip at x0 + depth, arms half_h above and below, rounded stroke of width `thick`
    tipx = x0 + depth
    return min(sd_segment(px, py, x0, cy - half_h, tipx, cy), sd_segment(px, py, x0, cy + half_h, tipx, cy)) - thick / 2


def render(size, variant):
    c0, c1, fg, lead_a, ghost_a = VARIANTS[variant]
    s = float(size)
    # strokes thicken relatively at small sizes so they survive 16 px
    thick = s * (0.135 if size <= 20 else 0.115 if size <= 32 else 0.10)
    inset = 0.5 if size <= 24 else s * 0.02
    radius = s * 0.24
    half_h, depth = s * 0.20, s * 0.17
    lead_x, ghost_x = s * 0.525, s * 0.305   # the pair spans ~0.25-0.75: optically centred
    cy = s * 0.5
    px_out = bytearray()
    for y in range(size):
        for x in range(size):
            fx, fy = x + 0.5, y + 0.5
            # badge coverage (1 px antialias band)
            d_badge = sd_round_box(fx, fy, s / 2, s / 2, s / 2 - inset, s / 2 - inset, radius)
            a_badge = max(0.0, min(1.0, 0.5 - d_badge))
            if a_badge <= 0:
                px_out += b'\0\0\0\0'
                continue
            g = max(0.0, min(1.0, (fx + fy) / (2 * s)))   # diagonal gradient
            r, gg, b = (c0[i] + (c1[i] - c0[i]) * g for i in range(3))
            # subtle top highlight
            hl = max(0.0, 1.0 - fy / (s * 0.55)) * 0.10
            r, gg, b = r + (255 - r) * hl, gg + (255 - gg) * hl, b + (255 - b) * hl
            # chevrons: ghost (generated frame) under lead (real frame)
            for x0, alpha in ((ghost_x, ghost_a), (lead_x, lead_a)):
                d = sd_chevron(fx, fy, x0, half_h, depth, cy, thick)
                a = max(0.0, min(1.0, 0.5 - d)) * alpha
                r, gg, b = r + (fg[0] - r) * a, gg + (fg[1] - gg) * a, b + (fg[2] - b) * a
            px_out += bytes((int(r + 0.5), int(gg + 0.5), int(b + 0.5), int(a_badge * 255 + 0.5)))
    return bytes(px_out)   # RGBA, top-down


def png(size_w, size_h, rgba):
    raw = b''.join(b'\0' + rgba[y * size_w * 4:(y + 1) * size_w * 4] for y in range(size_h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size_w, size_h, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def ico(images):   # [(size, png bytes)] - PNG-compressed entries (Vista+; LoadImage reads them)
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b''.join(d for _, d in images)


def main():
    os.makedirs(ART, exist_ok=True)
    renders = {}
    for v in VARIANTS:
        imgs = []
        for sz in SIZES:
            px = render(sz, v)
            renders[(v, sz)] = px
            imgs.append((sz, png(sz, sz, px)))
        with open(os.path.join(ART, v + '.ico'), 'wb') as f:
            f.write(ico(imgs))
        print('wrote art/%s.ico (%s)' % (v, ', '.join(str(s) for s in SIZES)))
    # preview sheet: each variant at every size (except 256) on a light and a dark strip
    cell, rows = 72, len(VARIANTS) * 2
    small = [s for s in SIZES if s <= 64]
    W, H = cell * len(small), cell * rows
    sheet = bytearray(W * H * 4)
    for ri, (v, bg) in enumerate([(v, bg) for v in VARIANTS for bg in ((243, 244, 246), (32, 33, 36))]):
        for yy in range(cell):
            for xx in range(W):
                o = ((ri * cell + yy) * W + xx) * 4
                sheet[o:o + 4] = bytes((*bg, 255))
        for ci, sz in enumerate(small):
            px = renders[(v, sz)]
            ox, oy = ci * cell + (cell - sz) // 2, ri * cell + (cell - sz) // 2
            for y in range(sz):
                for x in range(sz):
                    sr, sg, sb, sa = px[(y * sz + x) * 4:(y * sz + x) * 4 + 4]
                    o = ((oy + y) * W + ox + x) * 4
                    a = sa / 255
                    for c, sv in enumerate((sr, sg, sb)):
                        sheet[o + c] = int(sheet[o + c] * (1 - a) + sv * a + 0.5)
    with open(os.path.join(ART, 'icons_preview.png'), 'wb') as f:
        f.write(png(W, H, bytes(sheet)))
    big = renders[('justflow_xe', 256)]
    with open(os.path.join(ART, 'justflow_xe_256.png'), 'wb') as f:
        f.write(png(256, 256, big))
    print('wrote art/icons_preview.png, art/justflow_xe_256.png')


if __name__ == '__main__':
    main()
