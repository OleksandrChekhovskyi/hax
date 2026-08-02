#!/usr/bin/env python3
"""Generate deterministic PNG fixtures for manual vision-pipeline tests.

The default fixtures test color, layout, counting, and block-letter OCR. ``--edge``
also creates images that the read tool must reject for exceeding dimension or byte
limits. Filenames intentionally do not reveal their expected answers.
"""

from __future__ import annotations

import argparse
import random
import struct
import sys
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

Rgb = tuple[int, int, int]
RowRenderer = Callable[[int], bytes]
PixelRenderer = Callable[[int, int], Rgb]

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
FIXTURE_NAMES = (
    "solid-color.png",
    "layout.png",
    "count-dots.png",
    "text-word.png",
    "edge-a.png",
    "edge-b.png",
)

# Only the glyphs needed by the fixtures are defined.
FONT = {
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
}


@dataclass(frozen=True)
class FixtureResult:
    name: str
    width: int
    height: int
    size_bytes: int
    prompt: str
    expected: str


def encode_png_chunk(tag: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", checksum)


def write_png(path: Path, width: int, height: int, row_bytes: RowRenderer) -> int:
    """Write an 8-bit RGB PNG and return its encoded size."""
    scanlines = bytearray()
    expected_row_bytes = width * 3
    for y in range(height):
        row = row_bytes(y)
        if len(row) != expected_row_bytes:
            raise ValueError(
                f"row {y} has {len(row)} bytes; expected {expected_row_bytes}"
            )
        scanlines.append(0)  # PNG filter type: none
        scanlines.extend(row)

    image_header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    encoded = (
        PNG_SIGNATURE
        + encode_png_chunk(b"IHDR", image_header)
        + encode_png_chunk(b"IDAT", zlib.compress(bytes(scanlines)))
        + encode_png_chunk(b"IEND", b"")
    )
    path.write_bytes(encoded)
    return len(encoded)


def rows_from_pixels(width: int, pixel_color: PixelRenderer) -> RowRenderer:
    def row_bytes(y: int) -> bytes:
        return bytes(channel for x in range(width) for channel in pixel_color(x, y))

    return row_bytes


def render_text_row(
    text: str,
    scale: int,
    font_row: int,
    foreground: Rgb = BLACK,
    background: Rgb = WHITE,
) -> bytes:
    foreground_bytes = bytes(foreground)
    background_bytes = bytes(background)
    rendered = bytearray()
    for font_column in range(len(text) * 6 - 1):
        glyph_column = font_column % 6
        pixel_on = (
            glyph_column < 5
            and FONT[text[font_column // 6]][font_row][glyph_column] == "1"
        )
        color = foreground_bytes if pixel_on else background_bytes
        rendered.extend(color * scale)
    return bytes(rendered)


def make_text_fixture(text: str, scale: int, margin: int) -> tuple[int, int, RowRenderer]:
    text_columns = len(text) * 6 - 1
    width = text_columns * scale + 2 * margin
    height = 7 * scale + 2 * margin
    blank_row = bytes(WHITE) * width
    horizontal_margin = bytes(WHITE) * margin

    def row_bytes(y: int) -> bytes:
        font_row = (y - margin) // scale
        if 0 <= font_row < 7:
            return (
                horizontal_margin
                + render_text_row(text, scale, font_row)
                + horizontal_margin
            )
        return blank_row

    return width, height, row_bytes


def make_dot_renderer(centers: tuple[tuple[int, int], ...], radius: int) -> PixelRenderer:
    def pixel_color(x: int, y: int) -> Rgb:
        for center_x, center_y in centers:
            if (x - center_x) ** 2 + (y - center_y) ** 2 <= radius**2:
                return BLACK
        return WHITE

    return pixel_color


def create_fixture(
    output_dir: Path,
    results: list[FixtureResult],
    name: str,
    width: int,
    height: int,
    row_bytes: RowRenderer,
    prompt: str,
    expected: str,
) -> None:
    size_bytes = write_png(output_dir / name, width, height, row_bytes)
    results.append(
        FixtureResult(name, width, height, size_bytes, prompt, expected)
    )


def create_standard_fixtures(
    output_dir: Path, results: list[FixtureResult]
) -> None:
    magenta_row = b"\xff\x00\xff" * 96
    create_fixture(
        output_dir,
        results,
        "solid-color.png",
        96,
        96,
        lambda _y: magenta_row,
        "The image is a single solid color. Name it.",
        "magenta",
    )

    halves_row = b"\xff\x00\x00" * 48 + b"\x00\x00\xff" * 48
    create_fixture(
        output_dir,
        results,
        "layout.png",
        96,
        96,
        lambda _y: halves_row,
        "Describe the colors and where each is located.",
        "red left half, blue right half",
    )

    dot_centers = ((24, 24), (104, 24), (64, 64), (24, 104), (104, 104))
    create_fixture(
        output_dir,
        results,
        "count-dots.png",
        128,
        128,
        rows_from_pixels(128, make_dot_renderer(dot_centers, radius=10)),
        "How many dots does the image contain?",
        "5",
    )

    width, height, row_bytes = make_text_fixture("HAX", scale=8, margin=8)
    create_fixture(
        output_dir,
        results,
        "text-word.png",
        width,
        height,
        row_bytes,
        "What text does the image show?",
        "HAX",
    )


def create_edge_fixtures(output_dir: Path, results: list[FixtureResult]) -> None:
    band_rows = (
        b"\xff\x00\x00" * 900,
        b"\x00\xff\x00" * 900,
        b"\x00\x00\xff" * 900,
    )
    create_fixture(
        output_dir,
        results,
        "edge-a.png",
        900,
        8400,
        lambda y: band_rows[y // 2800],
        "What does the image show?",
        "read refuses (dimension cap); after downscaling: red, green, and blue "
        "horizontal bands from top to bottom",
    )

    # Seeded noise keeps the source over the byte cap but shrinks below it when resized.
    white_pixel = bytes(WHITE)

    def edge_b_row(y: int) -> bytes:
        random_source = random.Random(0x48410000 + y)
        noise = bytes(random_source.getrandbits(8) for _ in range(1100 * 3))
        font_row = (y - 105) // 170
        if 0 <= font_row < 7:
            return (
                noise
                + white_pixel * 5
                + render_text_row("HAX", 170, font_row)
                + white_pixel * 5
            )
        return noise + white_pixel * 2900

    create_fixture(
        output_dir,
        results,
        "edge-b.png",
        4000,
        1400,
        edge_b_row,
        "What does the image show?",
        "read refuses (byte cap); after downscaling: HAX beside a noise strip",
    )


def print_results(output_dir: Path, results: list[FixtureResult]) -> None:
    name_width = max(len(result.name) for result in results)
    print(f"wrote {len(results)} fixture(s) to {output_dir}\n")
    for result in results:
        dimensions = f"{result.width}x{result.height}"
        print(
            f"  {result.name:<{name_width}}  {dimensions:>9}  "
            f"{result.size_bytes:>8} bytes"
        )
        print(f"  {'':<{name_width}}  prompt: {result.prompt}")
        print(f"  {'':<{name_width}}  expect: {result.expected}\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dir",
        dest="output_dir",
        type=Path,
        default=Path("/tmp/hax-vision-fixtures"),
    )
    parser.add_argument(
        "--edge",
        action="store_true",
        help="also generate fixtures that exceed read-tool limits",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir: Path = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    for name in FIXTURE_NAMES:
        (output_dir / name).unlink(missing_ok=True)

    results: list[FixtureResult] = []
    create_standard_fixtures(output_dir, results)
    if args.edge:
        create_edge_fixtures(output_dir, results)
    print_results(output_dir, results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
