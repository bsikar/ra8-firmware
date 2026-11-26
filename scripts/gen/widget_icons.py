#!/usr/bin/env python3
"""Generate the packed 2-bit nav icon atlas from the checked-in SVG sources."""

from __future__ import annotations

import argparse
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "libs/ra8_widget/src/icons"
OUTPUT = ROOT / "libs/ra8_widget/src/icons_atlas.bin"
NAMES = ("back", "chevron_right", "play", "pause", "home", "library", "music", "settings", "search")
SIZE = 16
SCALE = 4


def number_list(value: str) -> list[float]:
    """Parse whitespace- or comma-separated SVG coordinate values."""
    return [float(item) for item in value.replace(",", " ").split()]


def render(path: Path) -> bytes:
    """Rasterize one checked-in monochrome icon SVG into packed 2-bit pixels."""
    root = ET.parse(path).getroot()  # noqa: S314 -- parses checked-in icon SVG sources only
    width, height = number_list(root.attrib.get("viewBox", "0 0 16 16"))[2:]
    image = Image.new("L", (SIZE * SCALE, SIZE * SCALE), 255)
    draw = ImageDraw.Draw(image)
    sx, sy = SIZE * SCALE / width, SIZE * SCALE / height
    for shape in root:
        tag = shape.tag.rsplit("}", 1)[-1]
        fill_attr = shape.attrib.get("fill", "#000").lower()
        color = 0 if fill_attr not in ("none", "white", "#fff", "#ffffff") else 255
        if tag == "polygon":
            points = number_list(shape.attrib["points"])
            poly = [(points[i] * sx, points[i + 1] * sy) for i in range(0, len(points), 2)]
            draw.polygon(poly, fill=color)
        elif tag == "rect":
            x, y = float(shape.get("x", 0)), float(shape.get("y", 0))
            w, h = float(shape.attrib["width"]), float(shape.attrib["height"])
            draw.rectangle((x * sx, y * sy, (x + w) * sx - 1, (y + h) * sy - 1), fill=color)
        elif tag == "circle":
            cx, cy, radius = (float(shape.attrib[key]) for key in ("cx", "cy", "r"))
            bbox = (
                (cx - radius) * sx,
                (cy - radius) * sy,
                (cx + radius) * sx,
                (cy + radius) * sy,
            )
            draw.ellipse(bbox, fill=color)
        else:
            msg = f"unsupported SVG element {tag} in {path.name}"
            raise ValueError(msg)
    small = image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    levels = [min(3, (255 - pixel) * 3 // 255) for pixel in small.tobytes()]
    packed = bytearray()
    for offset in range(0, len(levels), 4):
        packed.append(sum(levels[offset + bit] << (6 - 2 * bit) for bit in range(4)))
    return bytes(packed)


def generate() -> bytes:
    """Build the R8IA widget icon atlas header and packed glyph records."""
    records = b"".join(render(SOURCE / f"{name}.svg") for name in NAMES)
    return b"R8IA" + struct.pack("<BBH", 1, len(NAMES), SIZE) + records


def main() -> int:
    """Write or verify the checked-in widget icon atlas."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail when the packed atlas is stale")
    args = parser.parse_args()
    data = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_bytes() != data:
            print(f"stale widget icon atlas: {OUTPUT.relative_to(ROOT)}")
            return 1
        print(f"{len(NAMES)} icons current ({len(data)} bytes)")
        return 0
    OUTPUT.write_bytes(data)
    print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
