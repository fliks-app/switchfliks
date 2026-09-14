#!/usr/bin/env python3
"""Renders the Fliks wordmark into romfs/logo.png.

ImageMagick's SVG renderer ignores the CSS <style> block the brand asset uses
for its fills and cannot resolve url(#gradient) references, so both are
inlined first: each class becomes presentation attributes, and each gradient
collapses to the colour at its 50% stop. The mark keeps its hue; only the
along-the-path shading is lost, which is invisible at the sizes the app
draws it.

    python3 tools/build_logo.py external/fliks/client/public/fliks-logo-ondark.svg
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HEIGHT = 96


def inline(src_text: str) -> str:
    text = src_text.replace("Dégradé_sans_nom_", "grad")
    text = text.replace(' filter="url(#vibrant)"', "")

    style = re.search(r"<style>(.*?)</style>", text, re.S)
    rules = {}
    if style:
        for rule in re.finditer(r"\.([\w-]+)\s*\{(.*?)\}", style.group(1), re.S):
            props = re.findall(r"([\w-]+)\s*:\s*([^;]+);?", rule.group(2))
            rules[rule.group(1)] = {k.strip(): v.strip() for k, v in props}
        text = re.sub(r"<style>.*?</style>", "", text, flags=re.S)

    def expand(match):
        props = rules.get(match.group(1), {})
        return " " + " ".join(f'{k}="{v}"' for k, v in props.items())

    text = re.sub(r'\sclass="([\w-]+)"', expand, text)
    text = bake_rect_transforms(text)

    for grad in re.finditer(r'<linearGradient id="([\w-]+)"(.*?)</linearGradient>', text, re.S):
        stops = [(float(o), c) for o, c in re.findall(
            r'offset="([\d.]+)"\s+stop-color="(#[0-9a-fA-F]{6})"', grad.group(2))]
        if stops:
            mid = min(stops, key=lambda s: abs(s[0] - 0.5))[1]
            text = text.replace(f"url(#{grad.group(1)})", mid)
    return text


ATTR = re.compile(r'([\w-]+)\s*=\s*"([^"]*)"')


def bake_rect_transforms(text: str) -> str:
    """<rect transform="translate(..) rotate(..)"> becomes an explicit polygon.

    Two letters of the wordmark are rotated rects, and MSVG drops the
    transform rather than applying it, which loses the "li" of Fliks.
    """
    import math

    def convert(match):
        attrs = dict(ATTR.findall(match.group(0)))
        transform = attrs.pop("transform", "")
        if "rotate" not in transform:
            return match.group(0)

        nums = lambda key: float(attrs.pop(key, 0) or 0)
        x, y, w, h = nums("x"), nums("y"), nums("width"), nums("height")

        tx = ty = 0.0
        tr = re.search(r"translate\(\s*([-\d.]+)[\s,]+([-\d.]+)\s*\)", transform)
        if tr:
            tx, ty = float(tr.group(1)), float(tr.group(2))
        rot = re.search(r"rotate\(\s*([-\d.]+)", transform)
        angle = math.radians(float(rot.group(1))) if rot else 0.0
        ca, sa = math.cos(angle), math.sin(angle)

        # translate(t) rotate(a) applies the rotation to the point first.
        def place(px, py):
            return (tx + px * ca - py * sa, ty + px * sa + py * ca)

        corners = [place(x, y), place(x + w, y), place(x + w, y + h), place(x, y + h)]
        points = " ".join(f"{px:.3f},{py:.3f}" for px, py in corners)
        rest = " ".join(f'{k}="{v}"' for k, v in attrs.items())
        return f'<polygon {rest} points="{points}"/>'

    return re.sub(r"<rect\b[^>]*/?>", convert, text)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    out = Path(__file__).resolve().parent.parent / "romfs" / "logo.png"
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as tmp:
        tmp.write(inline(src.read_text(encoding="utf-8")))
        staged = tmp.name

    subprocess.run(
        ["magick", "-background", "none", "-density", "900", staged,
         "-resize", f"x{HEIGHT}", "PNG32:" + str(out)],
        check=True,
    )
    Path(staged).unlink()
    print(f"{out} written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
