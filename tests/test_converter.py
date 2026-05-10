"""
test_converter.py — End-to-end tests for the iwconv CLI tool.

Tests verify:
  - PNG → IWI → PNG round-trip is pixel-exact for RGB8 images.
  - PNG → IWI → PNG round-trip is pixel-exact for RGBA8 images.
  - PNG → IWI → PNG round-trip is pixel-exact for 16bpc (high-bit-depth) PNGs.
  - iwconv produces a non-zero error code for invalid input files.

Usage
-----
Set IWCONV_PATH to the path of the iwconv binary, or put it on PATH:

    IWCONV_PATH=/path/to/build/src/tools/iwconv pytest tests/

Run directly with python:

    IWCONV_PATH=... python -m pytest tests/test_converter.py -v
"""

import os
import struct
import subprocess
import sys
import zlib

import pytest

# ─── Locate iwconv binary ─────────────────────────────────────────────────────

def _find_iwconv() -> str:
    """Return the path to the iwconv binary, or skip if not found."""
    path = os.environ.get("IWCONV_PATH", "iwconv")
    try:
        subprocess.run(
            [path, "--help"],
            capture_output=True,
            timeout=10,
        )
        # iwconv may exit non-zero when called with --help; that is fine.
        return path
    except FileNotFoundError:
        pytest.skip(
            f"iwconv not found at {path!r} — set IWCONV_PATH env var",
            allow_module_level=True,
        )


IWCONV = _find_iwconv()

# ─── Minimal PNG writer (no external library required) ────────────────────────

def _png_chunk(tag: bytes, data: bytes) -> bytes:
    """Pack a single PNG chunk (length + tag + data + CRC)."""
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def make_png(
    width: int,
    height: int,
    pixels: bytes,
    bit_depth: int = 8,
    color_type: int = 2,  # 2 = RGB, 6 = RGBA
) -> bytes:
    """
    Assemble a minimal PNG file from raw pixel data.

    pixels must be a flat byte string in the correct bit/channel layout:
      - RGB8   : width * height * 3 bytes
      - RGBA8  : width * height * 4 bytes
      - RGB16  : width * height * 6 bytes, big-endian samples
      - RGBA16 : width * height * 8 bytes, big-endian samples
    """
    channels = {2: 3, 6: 4}.get(color_type, 3)
    bytes_per_sample = bit_depth // 8
    stride = width * channels * bytes_per_sample

    # Build raw image data: prepend a 0x00 filter byte to each row.
    raw = b""
    for y in range(height):
        raw += b"\x00" + pixels[y * stride : (y + 1) * stride]

    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    idat = zlib.compress(raw, level=6)

    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", idat)
        + _png_chunk(b"IEND", b"")
    )


def read_png(data: bytes):
    """
    Parse a minimal PNG file and return (width, height, bit_depth, channels, pixels).

    pixels is a flat list of integer sample values in row-major order.
    Only supports unfiltered (filter type 0) IDAT chunks.
    """
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "Not a PNG file"
    pos = 8
    ihdr_data = None
    idat_data = b""

    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        tag = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        pos += 12 + length

        if tag == b"IHDR":
            ihdr_data = chunk_data
        elif tag == b"IDAT":
            idat_data += chunk_data
        elif tag == b"IEND":
            break

    assert ihdr_data is not None
    width, height = struct.unpack(">II", ihdr_data[:8])
    bit_depth = ihdr_data[8]
    color_type = ihdr_data[9]
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]

    raw = zlib.decompress(idat_data)
    bytes_per_sample = bit_depth // 8
    stride = width * channels * bytes_per_sample

    samples = []
    for y in range(height):
        # Skip the filter byte (we only support filter type 0 here).
        row = raw[y * (stride + 1) + 1 : y * (stride + 1) + 1 + stride]
        if bytes_per_sample == 1:
            samples.extend(row)
        else:
            for i in range(0, len(row), bytes_per_sample):
                value = int.from_bytes(row[i : i + bytes_per_sample], "big")
                samples.append(value)

    return width, height, bit_depth, channels, samples


# ─── Helpers ──────────────────────────────────────────────────────────────────

def run_iwconv(args: list[str], *, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(
        [IWCONV] + args,
        capture_output=True,
        timeout=timeout,
    )


# ─── Tests ────────────────────────────────────────────────────────────────────

class TestRGBRoundTrip:
    """PNG → IWI → PNG round-trip tests for various colour depths."""

    def test_rgb8_roundtrip(self, tmp_path):
        """RGB8 (24bpp) PNG survives a PNG→IWI→PNG round-trip unchanged."""
        width, height = 16, 16

        # Synthetic gradient: R increases across columns, G across rows.
        pixels = bytes(
            (x * 16) % 256 if c == 0 else (y * 16) % 256 if c == 1 else 64
            for y in range(height)
            for x in range(width)
            for c in range(3)
        )
        src_png = make_png(width, height, pixels, bit_depth=8, color_type=2)

        src_file = tmp_path / "src.png"
        iwi_file = tmp_path / "out.iwi"
        dst_file = tmp_path / "dst.png"

        src_file.write_bytes(src_png)

        # PNG → IWI
        r = run_iwconv([str(src_file), str(iwi_file)])
        assert r.returncode == 0, f"PNG→IWI failed:\n{r.stderr.decode(errors='replace')}"

        # IWI → PNG
        r = run_iwconv([str(iwi_file), str(dst_file)])
        assert r.returncode == 0, f"IWI→PNG failed:\n{r.stderr.decode(errors='replace')}"

        # Compare
        _, _, bit_depth, channels, out_samples = read_png(dst_file.read_bytes())
        assert bit_depth == 8
        assert channels == 3
        assert list(pixels) == out_samples, "RGB8 round-trip is not pixel-exact"

    def test_rgba8_roundtrip(self, tmp_path):
        """RGBA8 (32bpp) PNG survives a round-trip unchanged."""
        width, height = 8, 8

        pixels = bytes(
            (x * 32) % 256 if c == 0
            else (y * 32) % 256 if c == 1
            else 128 if c == 2
            else 200        # alpha
            for y in range(height)
            for x in range(width)
            for c in range(4)
        )
        src_png = make_png(width, height, pixels, bit_depth=8, color_type=6)

        src_file = tmp_path / "src.png"
        iwi_file = tmp_path / "out.iwi"
        dst_file = tmp_path / "dst.png"

        src_file.write_bytes(src_png)

        r = run_iwconv([str(src_file), str(iwi_file)])
        assert r.returncode == 0, f"PNG→IWI failed:\n{r.stderr.decode(errors='replace')}"

        r = run_iwconv([str(iwi_file), str(dst_file)])
        assert r.returncode == 0, f"IWI→PNG failed:\n{r.stderr.decode(errors='replace')}"

        _, _, bit_depth, channels, out_samples = read_png(dst_file.read_bytes())
        assert bit_depth == 8
        assert channels == 4
        assert list(pixels) == out_samples, "RGBA8 round-trip is not pixel-exact"

    def test_rgb16_roundtrip(self, tmp_path):
        """
        16bpc RGB PNG (48bpp, analogous to '30-bit' HDR capture) survives
        a round-trip unchanged.

        16-bit samples are stored big-endian in PNG.
        """
        width, height = 4, 4

        # Samples as 16-bit integers (0–65535).
        sample_values = [
            (x * 4096 + y * 256) % 65536
            for y in range(height)
            for x in range(width)
            for _ in range(3)
        ]
        # Encode as big-endian bytes for PNG.
        pixels = b"".join(struct.pack(">H", v) for v in sample_values)

        src_png = make_png(width, height, pixels, bit_depth=16, color_type=2)

        src_file = tmp_path / "src16.png"
        iwi_file = tmp_path / "out16.iwi"
        dst_file = tmp_path / "dst16.png"

        src_file.write_bytes(src_png)

        r = run_iwconv([str(src_file), str(iwi_file)])
        assert r.returncode == 0, f"PNG→IWI failed:\n{r.stderr.decode(errors='replace')}"

        r = run_iwconv([str(iwi_file), str(dst_file)])
        assert r.returncode == 0, f"IWI→PNG failed:\n{r.stderr.decode(errors='replace')}"

        _, _, bit_depth, channels, out_samples = read_png(dst_file.read_bytes())
        assert bit_depth == 16
        assert channels == 3
        assert sample_values == out_samples, "RGB16 round-trip is not pixel-exact"

    def test_single_pixel_image(self, tmp_path):
        """1×1 pixel images are valid edge cases."""
        pixels = bytes([100, 150, 200])
        src_png = make_png(1, 1, pixels, bit_depth=8, color_type=2)

        src_file = tmp_path / "1x1.png"
        iwi_file = tmp_path / "1x1.iwi"
        dst_file = tmp_path / "1x1_out.png"

        src_file.write_bytes(src_png)

        r = run_iwconv([str(src_file), str(iwi_file)])
        assert r.returncode == 0

        r = run_iwconv([str(iwi_file), str(dst_file)])
        assert r.returncode == 0

        _, _, _, _, out_samples = read_png(dst_file.read_bytes())
        assert list(pixels) == out_samples


class TestErrorHandling:
    """iwconv must reject invalid inputs gracefully."""

    def test_nonexistent_input_file(self, tmp_path):
        r = run_iwconv([str(tmp_path / "no_such_file.png"),
                        str(tmp_path / "out.iwi")])
        assert r.returncode != 0, "Expected non-zero exit for missing input"

    def test_invalid_png_content(self, tmp_path):
        bad_file = tmp_path / "bad.png"
        bad_file.write_bytes(b"This is not a PNG file at all.")
        r = run_iwconv([str(bad_file), str(tmp_path / "out.iwi")])
        assert r.returncode != 0, "Expected non-zero exit for corrupt input"

    def test_invalid_iwi_content(self, tmp_path):
        bad_iwi = tmp_path / "bad.iwi"
        bad_iwi.write_bytes(b"\x00\x01\x02\x03 not an IWI file")
        r = run_iwconv([str(bad_iwi), str(tmp_path / "out.png")])
        assert r.returncode != 0, "Expected non-zero exit for corrupt IWI"

    def test_no_arguments(self):
        r = run_iwconv([])
        assert r.returncode != 0, "Expected non-zero exit when no arguments given"
