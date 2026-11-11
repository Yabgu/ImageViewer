# JPEG loader ABI fixtures

Five synthetic baseline JPEGs, checked in as binaries, used by
`../test_imageloader_jpeg.cpp` to drive the built `ImageLoaderJpeg` plugin
through its C ABI. Four are 16x16 with a known component count and uniform
pixel content; the fifth is 16x8 with a two-tone raster and an EXIF
Orientation tag.

| File | Components | Colour space | Bytes | Meaning |
|---|---|---|---|---|
| `gray_1comp.jpg` | 1 | grayscale | 333 | accepted; described as `GRAY` at 8 bits |
| `ycbcr_3comp.jpg` | 3 | YCbCr | 632 | accepted; described as `R`/`G`/`B` at 8/8/8 |
| `cmyk_4comp.jpg` | 4 | CMYK + Adobe APP14 | 362 | rejected: `JPEG has 4 components; ...` |
| `two_comp.jpg` | 2 | `JCS_UNKNOWN` | 323 | rejected: `JPEG has 2 components; ...` |
| `orient6_3comp.jpg` | 3 | YCbCr + EXIF APP1 | 669 | accepted as 8x16, upright (see below) |

All samples of the first four are uniform: 128 for the 1-, 2- and 3-component
files. The CMYK file stores 255, which under the Adobe convention is
*inverted*, so it means "no ink" — white, not mid-grey. That detail is
invisible to the tests because the loader rejects 4-component input before
converting anything.

## The oriented fixture

`orient6_3comp.jpg` exists so the EXIF-orientation path has a real file to run
against. It stores a 16x8 landscape raster — left half white, right half black
— and declares `Orientation = 6` (rotate 90° clockwise) in an APP1 segment
immediately after SOI, exactly the shape a phone camera writes for a portrait
photo. The loader must apply the tag and return **8x16, white half on top**;
before the tag was honoured this file loaded sideways with `width == 16`.

The halves cover whole 8x8 DCT blocks, so the decoded values stay flat and the
test can hold them to a tight tolerance. The tag lives in the file, not in the
test: this is the only fixture whose correctness depends on bytes outside the
JPEG entropy stream.

## Regenerating

`generate_fixtures.c` reproduces all five files **byte-for-byte** with
libjpeg-turbo 8:

```bash
cc -O2 -o /tmp/gen_fixtures generate_fixtures.c -ljpeg
mkdir -p /tmp/fx && /tmp/gen_fixtures /tmp/fx
for f in gray_1comp ycbcr_3comp cmyk_4comp two_comp orient6_3comp; do
    cmp /tmp/fx/$f.jpg $f.jpg && echo "ok $f"
done
```

A different libjpeg major version may encode the entropy-coded segment
differently while producing a structurally identical file. That is acceptable:
the tests depend on the component count, the EXIF tag, and the flat halves, not
on the exact bytes.

The generator encodes to a memory buffer (`jpeg_mem_dest`) and writes it
itself. Observed on Windows: handing the executable's `FILE*` to the vcpkg
libjpeg-turbo DLL makes `jpeg_finish_compress` fastfail inside the CRT's
`fflush`. The memory form is what the APP1 splice needs anyway.

## Why `two_comp.jpg` will not open in most image viewers

It is not corrupt. libjpeg decodes it with no errors and no warnings: 2
components, 16x16, all 512 samples exactly 128. It is unviewable because
2-component JPEG has no standard colour space, and decoders therefore refuse to
map it onto an output image:

| Tool | Result |
|---|---|
| libjpeg `jpeg_start_decompress` with native colour space | OK, 2 components |
| libjpeg forced to `JCS_RGB` (what Qt does) | `Unsupported color conversion request` |
| Qt / Gwenview (`QImageReader`) | `Unable to read image data` |
| Pillow | `UnidentifiedImageError` |
| ImageMagick `identify` | reports `JPEG 16x16 8-bit sRGB` |
| ImageMagick `magick` (convert) | `image type not supported` |

Measured through Qt 6's JPEG plugin — the one Gwenview uses:

```
gray_1comp.jpg    OK      Format_Grayscale8
ycbcr_3comp.jpg   OK      Format_RGB32
cmyk_4comp.jpg    OK      Format_CMYK8888
two_comp.jpg      FAILED  "Unable to read image data"
```

Qt requests RGB output for any JPEG that is not CMYK/YCCK. Converting 2
components to 3 is not something libjpeg can do, so `jpeg_start_decompress`
fails and the plugin reports a generic read error. No 2-component JPEG opens in
Gwenview, regardless of how it is encoded.

That is precisely why the file is useful: it is the only way to exercise the
`num_components == 2` branch of the loader's component-count policy. Making it
viewable would mean giving it a different component count, which would delete
the coverage.

## Format fixtures (PNG, WebP, TIFF, BMP, GIF, IWI)

The seven files below are the inputs for the per-format ABI tests
(`../test_imageloader_{png,webp,tiff,stb,gif,iwi}.cpp`) and for
`../test_imageloader_memory.cpp`. They exist for one property: the same bytes
must decode identically through `LoadImageFromFile` and `LoadImageFromMemory`.

All of them encode the same 4x4 RGB pattern, so the numbers the tests assert are
the same for every format:

    pixel (x, y):  R = 16 + 16*x,  G = 32 + 16*y,  B = 200 - 16*(x + y)

The PNG additionally carries a per-pixel alpha ramp (`A = 240 + x`), so the
RGBA8 path has an alpha that is not 255 to compare.

| File | Format | Components | Bytes | Notes |
|---|---|---|---|---|
| `rgba_4x4.png` | PNG, 8-bit | RGBA | 99 | explicit sRGB chunk, so colour space is not a heuristic |
| `lossless_4x4.webp` | WebP, VP8L | RGBA | 102 | lossless: a lossy encode would make the samples encoder-dependent |
| `rgb_4x4.tif` | TIFF, uncompressed | RGB | 200 | top-left origin, contig planar config |
| `rgb_4x4.bmp` | BMP, 24-bit BI_RGB | RGB | 102 | stored bottom-up, so it also checks the row flip |
| `gif_4x4.gif` | GIF89a, 16-entry palette | RGBA in memory | 96 | palette index i is pixel i; no transparency, frame covers the canvas |
| `rgba_4x4.iwi` | IWI, interleaved | RGBA | 138 | 74-byte header + tightly packed payload; exercises the RGBA path |
| `rgb_4x4.iwi` | IWI, interleaved | RGB | 122 | the same header with 3 components; no alpha is invented |

They are checked in as binaries and regenerated by `generate_format_fixtures.c`:

```bash
cc -O2 -o /tmp/gen_formats generate_format_fixtures.c \
    $(pkg-config --cflags --libs libpng libwebp libtiff-4)
mkdir -p /tmp/fx && /tmp/gen_formats /tmp/fx
for f in rgba_4x4.png lossless_4x4.webp rgb_4x4.tif rgb_4x4.bmp gif_4x4.gif \
         rgba_4x4.iwi rgb_4x4.iwi; do
    cmp /tmp/fx/$f $f && echo "ok $f"
done
```

Byte-identical reproduction was confirmed with libpng 1.6.59, libwebp 1.6.0 and
libtiff 4.7.2 on Linux x86-64. A different libpng or libwebp major version may
compress differently while producing a structurally identical image; the tests
depend on the decoded samples, not on the encoded bytes, so that is harmless —
but the `cmp` loop above will then report a difference, which is expected.

The BMP and the GIF are written by the generator by hand rather than by a
library: the BMP is a 54-byte header and bottom-up BGR rows, and the GIF is a
palettized colour table plus LZW framing. Emitting ~100 bytes directly is
easier to reproduce than adding a dependency, and for GIF it also keeps the
fixture from depending on whichever encoder the machine building it happened
to have.

The GIF's LZW deliberately does not compress: the encoder emits a CLEAR code
before every literal, so the decoder's dictionary resets each time and the code
width never changes from 5 bits. That removes the classic hand-written-encoder
bug -- encoder and decoder disagreeing about the exact code at which the width
grows -- for about ten extra bytes.

### Truncation behaviour differs by decoder

Not a fixture concern, but the reason the per-format tests assert different
things about truncated input:

- PNG, WebP, TIFF and JPEG refuse a span cut in the header.
- stb_image decodes a BMP whose pixel data is cut short *without* reporting an
  error, and does so identically through the file path. The stb test therefore
  asserts "no crash, and the descriptor still matches the buffer" rather than
  "truncation is refused" — the latter is stb's behaviour to change.
