#!/usr/bin/env python3
"""Bakes the Lucide icons the Fliks client uses into one alpha atlas.

The web client renders them as `<svg lucideX>`; here they become a single
8-bit coverage texture that the UI shader samples in its alpha-mask mode and
tints, so an icon costs one quad and never breaks the batch.

Lucide icons are stroked outlines with round caps and joins, so the raster is
a distance field: flatten every path to polylines, then shade each pixel by
its distance to the nearest segment. That needs no SVG library — ImageMagick
only rasterises SVG when rsvg-convert is installed, and cairosvg is not a
dependency worth adding for a build step that runs about once.

    python3 tools/build_icons.py path/to/lucide-static/icons
"""
import math
import re
import struct
import sys
import zlib
from pathlib import Path

CELL = 96          # atlas cell, in pixels
VIEWBOX = 24.0     # every Lucide icon is a 24x24 viewBox
STROKE = 2.0       # ...stroked at 2 units
COLS = 8

# Order is the atlas index; keep in sync with ui::Icon in source/ui/Icons.h.
ICONS = [
    "play", "pause", "star", "check", "chevron-left", "chevron-right",
    "chevron-up", "chevron-down", "film", "heart", "search", "house",
    "library-big", "clapperboard", "tv-minimal", "settings", "circle-user-round",
    "list-plus", "plus", "x", "circle-x", "clock", "rocket", "cast",
    "menu", "pin", "ellipsis-vertical", "rotate-ccw", "arrow-left",
    "skip-forward", "skip-back", "loader-circle",
]

NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
CMD = re.compile(r"([MmLlHhVvCcSsQqTtAaZz])")


def numbers(text):
    return [float(n) for n in NUM.findall(text)]


def cubic(p0, p1, p2, p3, out, steps=16):
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        x = (u * u * u * p0[0] + 3 * u * u * t * p1[0]
             + 3 * u * t * t * p2[0] + t * t * t * p3[0])
        y = (u * u * u * p0[1] + 3 * u * u * t * p1[1]
             + 3 * u * t * t * p2[1] + t * t * t * p3[1])
        out.append((x, y))


def quad(p0, p1, p2, out, steps=12):
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        out.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))


def arc(p0, rx, ry, rot, large, sweep, p1, out, steps=24):
    """Endpoint-parameterised elliptical arc, per the SVG implementation notes."""
    if rx == 0 or ry == 0 or p0 == p1:
        out.append(p1)
        return
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rot)
    cos_p, sin_p = math.cos(phi), math.sin(phi)

    dx2 = (p0[0] - p1[0]) / 2.0
    dy2 = (p0[1] - p1[1]) / 2.0
    x1 = cos_p * dx2 + sin_p * dy2
    y1 = -sin_p * dx2 + cos_p * dy2

    lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
    if lam > 1.0:
        scale = math.sqrt(lam)
        rx *= scale
        ry *= scale

    denom = rx * rx * y1 * y1 + ry * ry * x1 * x1
    num = rx * rx * ry * ry - denom
    factor = math.sqrt(max(0.0, num / denom)) if denom else 0.0
    if large == sweep:
        factor = -factor
    cx1 = factor * rx * y1 / ry
    cy1 = -factor * ry * x1 / rx

    cx = cos_p * cx1 - sin_p * cy1 + (p0[0] + p1[0]) / 2.0
    cy = sin_p * cx1 + cos_p * cy1 + (p0[1] + p1[1]) / 2.0

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        det = ux * vy - uy * vx
        return math.atan2(det, dot)

    theta = angle(1.0, 0.0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry,
                  (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi

    for i in range(1, steps + 1):
        t = theta + delta * i / steps
        ex = rx * math.cos(t)
        ey = ry * math.sin(t)
        out.append((cos_p * ex - sin_p * ey + cx, sin_p * ex + cos_p * ey + cy))


def parse_path(d):
    """Returns a list of polylines in user units."""
    tokens = [t for t in CMD.split(d) if t.strip()]
    polys, cur = [], []
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    prev_ctrl = None
    prev_cmd = ""
    i = 0
    while i < len(tokens):
        cmd = tokens[i]
        i += 1
        args = numbers(tokens[i]) if i < len(tokens) and not CMD.fullmatch(tokens[i]) else []
        if args:
            i += 1
        rel = cmd.islower()
        c = cmd.upper()

        if c == "Z":
            if cur:
                cur.append(start)
                polys.append(cur)
                cur = []
            pos = start
            prev_cmd = c
            continue

        k = 0
        first = True
        while True:
            if c == "M":
                if k + 2 > len(args):
                    break
                x, y = args[k], args[k + 1]
                k += 2
                pos = (pos[0] + x, pos[1] + y) if rel else (x, y)
                if first:
                    if cur:
                        polys.append(cur)
                    cur = [pos]
                    start = pos
                    first = False
                else:
                    cur.append(pos)
            elif c in ("L", "H", "V"):
                if c == "L":
                    if k + 2 > len(args):
                        break
                    x, y = args[k], args[k + 1]
                    k += 2
                    pos = (pos[0] + x, pos[1] + y) if rel else (x, y)
                elif c == "H":
                    if k + 1 > len(args):
                        break
                    x = args[k]
                    k += 1
                    pos = (pos[0] + x, pos[1]) if rel else (x, pos[1])
                else:
                    if k + 1 > len(args):
                        break
                    y = args[k]
                    k += 1
                    pos = (pos[0], pos[1] + y) if rel else (pos[0], y)
                cur.append(pos)
            elif c in ("C", "S"):
                need = 6 if c == "C" else 4
                if k + need > len(args):
                    break
                if c == "C":
                    c1 = (args[k], args[k + 1])
                    c2 = (args[k + 2], args[k + 3])
                    end = (args[k + 4], args[k + 5])
                else:
                    c2 = (args[k], args[k + 1])
                    end = (args[k + 2], args[k + 3])
                    c1 = None
                k += need
                if rel:
                    if c == "C":
                        c1 = (pos[0] + c1[0], pos[1] + c1[1])
                    c2 = (pos[0] + c2[0], pos[1] + c2[1])
                    end = (pos[0] + end[0], pos[1] + end[1])
                if c1 is None:
                    c1 = pos if prev_ctrl is None or prev_cmd not in ("C", "S") else (
                        2 * pos[0] - prev_ctrl[0], 2 * pos[1] - prev_ctrl[1])
                cubic(pos, c1, c2, end, cur)
                prev_ctrl = c2
                pos = end
            elif c in ("Q", "T"):
                need = 4 if c == "Q" else 2
                if k + need > len(args):
                    break
                if c == "Q":
                    c1 = (args[k], args[k + 1])
                    end = (args[k + 2], args[k + 3])
                else:
                    c1 = None
                    end = (args[k], args[k + 1])
                k += need
                if rel:
                    if c1:
                        c1 = (pos[0] + c1[0], pos[1] + c1[1])
                    end = (pos[0] + end[0], pos[1] + end[1])
                if c1 is None:
                    c1 = pos if prev_ctrl is None or prev_cmd not in ("Q", "T") else (
                        2 * pos[0] - prev_ctrl[0], 2 * pos[1] - prev_ctrl[1])
                quad(pos, c1, end, cur)
                prev_ctrl = c1
                pos = end
            elif c == "A":
                if k + 7 > len(args):
                    break
                rx, ry, rot = args[k], args[k + 1], args[k + 2]
                large, sweep = bool(args[k + 3]), bool(args[k + 4])
                end = (args[k + 5], args[k + 6])
                k += 7
                if rel:
                    end = (pos[0] + end[0], pos[1] + end[1])
                arc(pos, rx, ry, rot, large, sweep, end, cur)
                pos = end
            else:
                break
            first = False
            if k >= len(args):
                break
        prev_cmd = c
        if c not in ("C", "S", "Q", "T"):
            prev_ctrl = None

    if cur:
        polys.append(cur)
    return polys


def ellipse_poly(cx, cy, rx, ry, steps=48):
    return [[(cx + rx * math.cos(2 * math.pi * i / steps),
              cy + ry * math.sin(2 * math.pi * i / steps))
             for i in range(steps + 1)]]


def rounded_rect_poly(x, y, w, h, r):
    if r <= 0:
        return [[(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)]]
    r = min(r, w / 2, h / 2)
    pts = []
    corners = [
        (x + w - r, y + r, -math.pi / 2, 0.0),
        (x + w - r, y + h - r, 0.0, math.pi / 2),
        (x + r, y + h - r, math.pi / 2, math.pi),
        (x + r, y + r, math.pi, 3 * math.pi / 2),
    ]
    for cx, cy, a0, a1 in corners:
        for i in range(9):
            a = a0 + (a1 - a0) * i / 8
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    pts.append(pts[0])
    return [pts]


def attrs(tag_text):
    return {m.group(1): m.group(2)
            for m in re.finditer(r'([\w-]+)\s*=\s*"([^"]*)"', tag_text)}


def shapes_of(svg_text):
    """Every drawable element flattened into polylines."""
    polys = []
    for tag, body in re.findall(r"<(path|circle|rect|line|polyline|polygon|ellipse)\b([^>]*)>",
                                svg_text):
        a = attrs(body)
        f = lambda key, default=0.0: float(a.get(key, default))
        if tag == "path":
            polys += parse_path(a.get("d", ""))
        elif tag == "circle":
            polys += ellipse_poly(f("cx"), f("cy"), f("r"), f("r"))
        elif tag == "ellipse":
            polys += ellipse_poly(f("cx"), f("cy"), f("rx"), f("ry"))
        elif tag == "rect":
            polys += rounded_rect_poly(f("x"), f("y"), f("width"), f("height"),
                                       f("rx", a.get("ry", 0)))
        elif tag == "line":
            polys.append([(f("x1"), f("y1")), (f("x2"), f("y2"))])
        elif tag in ("polyline", "polygon"):
            n = numbers(a.get("points", ""))
            pts = list(zip(n[0::2], n[1::2]))
            if tag == "polygon" and pts:
                pts.append(pts[0])
            if pts:
                polys.append(pts)
    return polys


def rasterize(polys, cell=CELL):
    """Coverage from the distance to the stroke skeleton; round caps and
    joins come for free because a distance field is what they describe."""
    scale = cell / VIEWBOX
    half = STROKE * scale / 2.0
    buf = bytearray(cell * cell)

    segments = []
    for poly in polys:
        for a, b in zip(poly, poly[1:]):
            ax, ay = a[0] * scale, a[1] * scale
            bx, by = b[0] * scale, b[1] * scale
            segments.append((ax, ay, bx, by, (bx - ax) ** 2 + (by - ay) ** 2))
        if len(poly) == 1:
            px, py = poly[0][0] * scale, poly[0][1] * scale
            segments.append((px, py, px, py, 0.0))

    reach = half + 1.0
    for ax, ay, bx, by, len2 in segments:
        x0 = max(0, int(math.floor(min(ax, bx) - reach)))
        x1 = min(cell - 1, int(math.ceil(max(ax, bx) + reach)))
        y0 = max(0, int(math.floor(min(ay, by) - reach)))
        y1 = min(cell - 1, int(math.ceil(max(ay, by) + reach)))
        dx, dy = bx - ax, by - ay
        for py in range(y0, y1 + 1):
            fy = py + 0.5
            row = py * cell
            for px in range(x0, x1 + 1):
                fx = px + 0.5
                if len2 > 0.0:
                    t = ((fx - ax) * dx + (fy - ay) * dy) / len2
                    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
                    qx, qy = ax + t * dx, ay + t * dy
                else:
                    qx, qy = ax, ay
                d = math.hypot(fx - qx, fy - qy)
                cov = half + 0.5 - d
                if cov <= 0.0:
                    continue
                v = 255 if cov >= 1.0 else int(cov * 255.0 + 0.5)
                if v > buf[row + px]:
                    buf[row + px] = v
    return buf


def write_png_gray(path, width, height, pixels):
    raw = b"".join(b"\x00" + bytes(pixels[y * width:(y + 1) * width]) for y in range(height))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    out = Path(__file__).resolve().parent.parent / "romfs" / "icons.png"
    out.parent.mkdir(parents=True, exist_ok=True)

    rows = (len(ICONS) + COLS - 1) // COLS
    width, height = COLS * CELL, rows * CELL
    atlas = bytearray(width * height)

    for index, name in enumerate(ICONS):
        svg = src / f"{name}.svg"
        if not svg.exists():
            print(f"missing icon: {name}")
            return 1
        cellbuf = rasterize(shapes_of(svg.read_text()))
        ox = (index % COLS) * CELL
        oy = (index // COLS) * CELL
        for y in range(CELL):
            dst = (oy + y) * width + ox
            atlas[dst:dst + CELL] = cellbuf[y * CELL:(y + 1) * CELL]
        print(f"  {index:2d} {name}")

    write_png_gray(out, width, height, atlas)
    print(f"{out}: {len(ICONS)} icons, {COLS}x{rows} grid of {CELL}px cells")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
