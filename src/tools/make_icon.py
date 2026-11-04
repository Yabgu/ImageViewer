#!/usr/bin/env python3
"""Build the Windows .ico from the master SVG artwork.

    python src/tools/make_icon.py [--svg PATH] [--out PATH] [--preview-dir DIR]
    python src/tools/make_icon.py --check

The SVG (assets/icon/iview.svg) is the only file edited by hand.  This script
renders every size Windows asks for with rsvg-convert, then packs them into a
single .ico:

  * 16..64 px  -> uncompressed 32-bit DIB entries (best compatibility, and the
                  sizes the shell and title bar actually use)
  * 128, 256   -> PNG-compressed entries (Vista+ supports these and it keeps
                  the file small)

Standard library only.  The one external tool is a rasterizer -- rsvg-convert
from librsvg, found on PATH, in the usual MSYS2 prefixes, or via RSVG_CONVERT.
Pillow is deliberately not needed: the rendered PNGs are decoded here.

`--check` reports whether the script can run in this environment and exits 0
when it can; CMake uses it to decide whether to generate the icon at build
time or fall back to the committed copy.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

DIB_SIZES = (16, 20, 24, 32, 40, 48, 64)
PNG_SIZES = (128, 256)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SVG = REPO_ROOT / "assets" / "icon" / "iview.svg"
DEFAULT_OUT = REPO_ROOT / "src" / "imageviewer" / "resources" / "iview.ico"

# rsvg-convert from an MSYS2 install is not always on PATH, so probe the
# standard prefixes too.  Running the executable by full path is enough: its
# MinGW runtime DLLs sit next to it.
_RSVG_CANDIDATES = (
    r"C:\msys64\ucrt64\bin\rsvg-convert.exe",
    r"C:\msys64\clang64\bin\rsvg-convert.exe",
    r"C:\msys64\mingw64\bin\rsvg-convert.exe",
    "/usr/bin/rsvg-convert",
    "/usr/local/bin/rsvg-convert",
)


def find_rsvg() -> str | None:
    """Return a usable rsvg-convert, or None."""
    override = os.environ.get("RSVG_CONVERT")
    if override:
        return override if Path(override).is_file() else None
    found = shutil.which("rsvg-convert")
    if found:
        return found
    for candidate in _RSVG_CANDIDATES:
        if Path(candidate).is_file():
            return candidate
    return None


def render(rsvg: str, svg: Path, size: int, out_png: Path) -> None:
    subprocess.run(
        [rsvg, "-w", str(size), "-h", str(size), str(svg), "-o", str(out_png)],
        check=True,
    )


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_png(blob: bytes) -> tuple[int, int, bytes]:
    """Decode an 8-bit RGBA, non-interlaced PNG into (width, height, RGBA)."""
    if blob[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    header = None
    idat = bytearray()
    while pos + 8 <= len(blob):
        (length,) = struct.unpack(">I", blob[pos : pos + 4])
        chunk = blob[pos + 4 : pos + 8]
        data = blob[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if chunk == b"IHDR":
            header = struct.unpack(">IIBBBBB", data)
        elif chunk == b"IDAT":
            idat += data
        elif chunk == b"IEND":
            break

    if header is None:
        raise ValueError("PNG has no IHDR")
    width, height, depth, color, _, _, interlace = header
    if (depth, color, interlace) != (8, 6, 0):
        raise ValueError(
            f"unsupported PNG (depth={depth} color={color} interlace={interlace});"
            " expected 8-bit RGBA non-interlaced"
        )

    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride

        if filter_type == 1:  # Sub
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                upleft = prev[i - 4] if i >= 4 else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"bad PNG filter type {filter_type} on row {y}")

        out[y * stride : (y + 1) * stride] = line
        prev = line

    return width, height, bytes(out)


def dib_entry(png_blob: bytes) -> bytes:
    """Encode a rendered PNG as the DIB payload an ICONDIR entry expects."""
    width, height, rgba = decode_png(png_blob)

    # 32-bit BGRA, bottom-up, no row padding.
    xor = bytearray(len(rgba))
    for y in range(height):
        src = (height - 1 - y) * width * 4
        dst = y * width * 4
        for x in range(width):
            i = src + x * 4
            j = dst + x * 4
            xor[j] = rgba[i + 2]      # B
            xor[j + 1] = rgba[i + 1]  # G
            xor[j + 2] = rgba[i]      # R
            xor[j + 3] = rgba[i + 3]  # A

    # AND mask: 1 bit per pixel, rows padded to 4 bytes.  Zero == opaque, and
    # the alpha channel already carries the real transparency.
    mask_stride = ((width + 31) // 32) * 4
    mask = bytes(mask_stride * height)

    # BITMAPINFOHEADER: height is doubled to cover the XOR image plus the mask.
    header = struct.pack(
        "<IiiHHIIiiII",
        40,                    # biSize
        width, height * 2,     # biWidth, biHeight
        1,                     # biPlanes
        32,                    # biBitCount
        0,                     # biCompression (BI_RGB)
        len(xor) + len(mask),  # biSizeImage
        0, 0, 0, 0,            # pels/meter, clrUsed, clrImportant
    )
    return header + bytes(xor) + mask


def build_ico(entries: list[tuple[int, bytes]]) -> bytes:
    count = len(entries)
    header = struct.pack("<HHH", 0, 1, count)

    offset = 6 + 16 * count
    directory = bytearray()
    payload = bytearray()
    for size, data in entries:
        dim = 0 if size >= 256 else size
        directory += struct.pack(
            "<BBBBHHII",
            dim, dim,    # width, height (0 means 256)
            0, 0,        # palette count, reserved
            1, 32,       # planes, bit count
            len(data), offset,
        )
        payload += data
        offset += len(data)

    return header + bytes(directory) + bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--svg", type=Path, default=DEFAULT_SVG)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument(
        "--preview-dir",
        type=Path,
        default=None,
        help="also keep the rendered PNGs in this directory",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="report whether this script can run here, then exit",
    )
    args = parser.parse_args()

    rsvg = find_rsvg()
    if args.check:
        if rsvg is None:
            print("rsvg-convert not found", file=sys.stderr)
            return 1
        print(rsvg)
        return 0
    if rsvg is None:
        sys.exit(
            "error: rsvg-convert not found.\n"
            "       Install librsvg (MSYS2: pacman -S mingw-w64-ucrt-x86_64-librsvg),\n"
            "       add it to PATH, or set RSVG_CONVERT to its full path."
        )
    if not args.svg.is_file():
        sys.exit(f"error: SVG not found: {args.svg}")

    entries: list[tuple[int, bytes]] = []

    with tempfile.TemporaryDirectory(prefix="iview-icon-") as tmp:
        tmp_dir = Path(tmp)
        for size in DIB_SIZES + PNG_SIZES:
            png = tmp_dir / f"iview-{size}.png"
            render(rsvg, args.svg, size, png)
            blob = png.read_bytes()
            entries.append((size, blob if size in PNG_SIZES else dib_entry(blob)))
            if args.preview_dir is not None:
                args.preview_dir.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(png, args.preview_dir / png.name)

    entries.sort(key=lambda entry: entry[0])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(build_ico(entries))

    print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")
    print("  sizes: " + ", ".join(str(size) for size, _ in entries))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
