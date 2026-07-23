#!/usr/bin/env python3
"""Generate deterministic image fixtures for testing model vision support.

Every major provider path (Anthropic image blocks, OpenAI Chat image_url,
Responses input_image, llama.cpp mmproj) can be smoke-tested by asking the
model to `read` one of these and answer a question only the pixels can
answer. Pure stdlib (zlib PNG writer) — no imaging dependencies, identical
bytes on every run, so failures are always the pipeline or the model, never
the fixture.

Usage:
    scripts/vision_fixtures.py [--dir DIR] [--edge]

Writes to --dir (default /tmp/hax-vision-fixtures) and prints, for each
file, the prompt to use and the expected answer. --edge additionally
generates oversized fixtures that must be *rejected* by the read tool
(dimension cap and byte cap) — useful for testing the downscale-hint error
path without a real photo.

The model sees the path in the tool call, so a filename must not leak
anything the accompanying prompt doesn't already state — solid-magenta.png
would hand over the answer, and an oversize fixture named big-noise.png
would let the model declare "too big" without ever calling the tool. Task
words the prompt itself uses (solid color, dots, text) are fine; the edge
fixtures get deliberately uninformative names. Expected answers live only
here and in the printed table.

Fixture expectations:
  solid-color.png     "The image is a single solid color. Name it."
                      -> magenta. The baseline pipeline check.
  layout.png          "Describe the colors and where each is located."
                      -> red left half, blue right half. Layout/orientation
                      check. Small or heavily quantized models sometimes
                      hallucinate extra stripes here; treat wrong *colors*
                      or wrong *order* as a pipeline bug, extra detail as
                      model quality.
  count-dots.png      "How many dots does the image contain?"
                      -> 5. Counting check (the count is not in the name);
                      also sensitive to accidental downscaling artifacts.
  text-word.png       "What text does the image show?"
                      -> HAX. Crude OCR check (block letters). For a
                      realistic text-heavy fixture use docs/screenshot.png,
                      which shows a real hax session.
  edge-a.png          (--edge) 900x8400 — read must REFUSE (8000px side
                      cap) with a downscale hint; the model must discover
                      this by calling read, not from the name. Content
                      survives the resize: after following the hint, the
                      image shows red/green/blue horizontal bands, so the
                      full refuse -> downscale -> re-read recovery loop is
                      testable end to end.
  edge-b.png          (--edge) 4000x1400, ~4.6MB — read must REFUSE (raw
                      size over the ~3.75MB cap; a seeded noise strip
                      makes the file incompressible). After downscaling,
                      the image shows the text HAX beside the noise strip.

Stale fixtures from previous runs (e.g. edge files after a run without
--edge) are removed from --dir first, so the directory always matches the
printed table exactly.

Other formats: the read tool sniffs magic bytes for PNG/JPEG/GIF/WebP, but
only PNG can be written dependency-free — convert a fixture with e.g.
`magick solid-color.png solid-color.webp` to cover the other parsers.
"""

import argparse
import os
import random
import struct
import sys
import zlib


def png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def write_png(path: str, width: int, height: int, row_at) -> int:
    """Write an 8-bit RGB PNG; row_at(y) returns 3*width bytes of RGB."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: none
        rows += row_at(y)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    blob = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(bytes(rows)))
        + png_chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


# 5x7 block font, just the letters the fixture needs.
FONT = {
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
}


def pixel_rows(width: int, pixel_at):
    """Adapt a per-pixel (x, y) -> (r, g, b) function to write_png's row form."""
    return lambda y: bytes(v for x in range(width) for v in pixel_at(x, y))


def text_row_bytes(text: str, scale: int, cy: int, fg=(0, 0, 0), bg=(255, 255, 255)) -> bytes:
    """One rendered row of `text` (font row cy, 0..6) at `scale`, as RGB bytes.
    Width is (len(text) * 6 - 1) * scale: 5-wide glyphs, 1-column gaps."""
    fg3, bg3 = bytes(fg), bytes(bg)
    out = bytearray()
    for cx in range(len(text) * 6 - 1):
        on = cx % 6 < 5 and FONT[text[cx // 6]][cy][cx % 6] == "1"
        out += (fg3 if on else bg3) * scale
    return bytes(out)


def text_fixture(text: str, scale: int, margin: int):
    cols = len(text) * 6 - 1
    width = cols * scale + 2 * margin
    height = 7 * scale + 2 * margin
    blank = b"\xff\xff\xff" * width
    pad = b"\xff\xff\xff" * margin

    def row(y: int) -> bytes:
        cy = (y - margin) // scale
        if 0 <= cy < 7:
            return pad + text_row_bytes(text, scale, cy) + pad
        return blank

    return width, height, row


def dots_pixel_fn(centers, radius: int):
    def at(x: int, y: int):
        for cx, cy in centers:
            if (x - cx) ** 2 + (y - cy) ** 2 <= radius**2:
                return (0, 0, 0)
        return (255, 255, 255)

    return at


# Every name this script has ever owned, for stale cleanup — the directory
# must always match the printed table, never mix runs.
ALL_FIXTURES = [
    "solid-color.png",
    "layout.png",
    "count-dots.png",
    "text-word.png",
    "edge-a.png",
    "edge-b.png",
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", default="/tmp/hax-vision-fixtures")
    ap.add_argument("--edge", action="store_true", help="also generate the oversized fixtures")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    for name in ALL_FIXTURES:
        try:
            os.remove(os.path.join(args.dir, name))
        except FileNotFoundError:
            pass

    made = []

    def emit(name, width, height, row_at, prompt, expect):
        path = os.path.join(args.dir, name)
        size = write_png(path, width, height, row_at)
        made.append((name, f"{width}x{height}", size, prompt, expect))

    magenta_row = b"\xff\x00\xff" * 96
    emit(
        "solid-color.png", 96, 96, lambda y: magenta_row,
        "The image is a single solid color. Name it.", "magenta",
    )
    halves_row = b"\xff\x00\x00" * 48 + b"\x00\x00\xff" * 48
    emit(
        "layout.png", 96, 96, lambda y: halves_row,
        "Describe the colors and where each is located.", "red left half, blue right half",
    )
    emit(
        "count-dots.png", 128, 128,
        pixel_rows(128, dots_pixel_fn([(24, 24), (104, 24), (64, 64), (24, 104), (104, 104)], 10)),
        "How many dots does the image contain?", "5",
    )
    w, h, row = text_fixture("HAX", scale=8, margin=8)
    emit("text-word.png", w, h, row, "What text does the image show?", "HAX")

    if args.edge:
        # Both edge fixtures carry content that survives the suggested
        # downscale, so the whole recovery loop is testable: read refuses
        # with a hint, the model resizes via bash, re-reads the copy, and
        # only then can answer the content question.
        bands = [b"\xff\x00\x00" * 900, b"\x00\xff\x00" * 900, b"\x00\x00\xff" * 900]
        emit(
            "edge-a.png", 900, 8400, lambda y: bands[y // 2800],
            "What does the image show?",
            "read refuses (per-side pixel cap) with a downscale hint; after downscaling: "
            "three horizontal bands — red, green, blue, top to bottom",
        )
        # Byte-cap trigger: ~4.6MB of seeded, incompressible noise in the
        # left strip; the rest is compressible text. The 4000px width keeps
        # the hint effective — at 1568px the noise shrinks ~6.5x in area,
        # putting the downscaled copy safely under the cap (pure noise at
        # the original size would still exceed it after a resize).
        rng = random.Random(0x4841)
        white = b"\xff\xff\xff"

        def edge_b_row(y: int) -> bytes:
            noise = bytes(rng.getrandbits(8) for _ in range(1100 * 3))
            cy = (y - 105) // 170
            if 0 <= cy < 7:
                return noise + white * 5 + text_row_bytes("HAX", 170, cy) + white * 5
            return noise + white * 2900

        emit(
            "edge-b.png", 4000, 1400, edge_b_row,
            "What does the image show?",
            "read refuses (byte cap) with a downscale hint; after downscaling: the text "
            "HAX beside a noise strip",
        )

    name_w = max(len(m[0]) for m in made)
    print(f"wrote {len(made)} fixture(s) to {args.dir}\n")
    for name, dims, size, prompt, expect in made:
        print(f"  {name:<{name_w}}  {dims:>9}  {size:>8} bytes")
        print(f"  {'':<{name_w}}  prompt: {prompt}")
        print(f"  {'':<{name_w}}  expect: {expect}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
