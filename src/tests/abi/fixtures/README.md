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
