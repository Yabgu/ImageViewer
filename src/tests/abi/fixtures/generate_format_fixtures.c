/*
 * generate_format_fixtures.c — regenerates the non-JPEG fixtures in this
 * directory.
 *
 * These are the inputs for the per-format ABI tests (test_imageloader_png.cpp,
 * _webp.cpp, _tiff.cpp, _stb.cpp, _iwi.cpp) and for test_imageloader_memory.cpp.  Their
 * job is not to probe format edges; it is to give each loader one small,
 * known-property file whose decoded bytes can be compared between the file
 * path and the memory path, and against the numbers asserted below.
 *
 *   cc -O2 -o /tmp/gen_formats generate_format_fixtures.c \
 *       $(pkg-config --cflags --libs libpng libwebp libtiff-4)
 *   mkdir -p /tmp/fx && /tmp/gen_formats /tmp/fx
 *   for f in rgba_4x4.png lossless_4x4.webp rgb_4x4.tif rgb_4x4.bmp gif_4x4.gif; do
 *     cmp /tmp/fx/$f $f && echo "ok $f"
 *   done
 *
 * All four fixtures encode the same 4x4 RGB pattern, so the RGB numbers the
 * tests assert are the same numbers for every format:
 *
 *   pixel (x, y): R = 16 + 16*x, G = 32 + 16*y, B = 200 - 16*(x + y)
 *
 * The PNG additionally carries a per-pixel alpha ramp (A = 240 + x), so the
 * RGBA8 path has a non-trivial alpha to compare.  WebP, TIFF and BMP fixtures
 * are RGB only.
 *
 * Encoders differ in what they can preserve:
 *   PNG   lossless, exact samples
 *   WebP  VP8L lossless (WebPEncodeLosslessRGBA) — a lossy VP8 encode would
 *         still be *self*-consistent between the two decode paths, but the
 *         exact sample values would then depend on the encoder build, and the
 *         tests assert them
 *   TIFF  uncompressed RGB, exact samples
 *   BMP   24-bit BI_RGB, exact samples
 *   GIF   palettized, exact samples (16 distinct colours, one per pixel)
 *
 * The BMP and the GIF are written by this program rather than by a library.
 * For the BMP the format is a 54-byte header and bottom-up BGR rows; for the
 * GIF it is the palettized + LZW framing below.  Pulling in a dependency to
 * emit ~100 bytes each would make this harder to reproduce, not easier, and
 * for GIF it would also make the fixture depend on whichever encoder the
 * whoever-built-it had installed.
 *
 * The GIF is lossless in the only sense that matters here: a 4x4 image has 16
 * pixels and this pattern has 16 distinct colours, so a 16-entry palette
 * represents every pixel exactly and the decoded RGB equals the other formats'
 * byte for byte.  It carries no alpha.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <tiffio.h>
#include <webp/encode.h>

#define W 4
#define H 4

static void fill_rgb(uint8_t *rgb) /* W*H*3, top-down */
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *p = rgb + (y * W + x) * 3;
            p[0] = (uint8_t)(16 + 16 * x);
            p[1] = (uint8_t)(32 + 16 * y);
            p[2] = (uint8_t)(200 - 16 * (x + y));
        }
    }
}

static void fill_rgba(uint8_t *rgba) /* W*H*4, top-down */
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *p = rgba + (y * W + x) * 4;
            p[0] = (uint8_t)(16 + 16 * x);
            p[1] = (uint8_t)(32 + 16 * y);
            p[2] = (uint8_t)(200 - 16 * (x + y));
            p[3] = (uint8_t)(240 + x);
        }
    }
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
}

/*
 * IWI: the 74-byte header ImageLoaderIWI parses, written field by field.
 *
 * `components` is 3 (RGB, opaque) or 4 (RGBA, with the alpha ramp).  Both
 * fixtures carry exactly the same RGB samples as the other formats' fixtures,
 * so the tests can hold every loader to the same numbers; the RGBA one adds
 * A = 240 + x, which is what distinguishes a loader that reads the alpha
 * component from one that synthesises 255.
 *
 * Semantics are written as R=1, G=2, B=3, A=4 and class as UNORM=2, matching
 * the container Tension's writer (apps/docs/tools/lib/iwi.mjs) produces.
 */
static int write_iwi_components(const char *path, int components)
{
    uint8_t buf[74 + W * H * 4];
    memset(buf, 0, 74);

    memcpy(buf + 0, "IWI1", 4);
    put_u32le(buf + 4, W);
    put_u32le(buf + 8, H);
    buf[12] = 0;                              /* sRGB */
    put_u16le(buf + 13, (uint16_t)components);
    put_u16le(buf + 15, (uint16_t)(components * 8));
    buf[17] = 0;                              /* interleaved */

    for (int i = 0; i < components; ++i) {
        uint8_t *rec = buf + 18 + i * 6;
        rec[0] = (uint8_t)(i + 1);            /* R, G, B[, A] */
        rec[1] = 2;                           /* UNORM */
        put_u16le(rec + 2, (uint16_t)(i * 8));
        put_u16le(rec + 4, 8);
    }

    const size_t bytes = (size_t)W * H * (size_t)components;
    put_u64le(buf + 66, (uint64_t)bytes);
    if (components == 4)
        fill_rgba(buf + 74);
    else
        fill_rgb(buf + 74);

    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    const size_t written = fwrite(buf, 1, 74 + bytes, f);
    fclose(f);
    return written == 74 + bytes ? 0 : 1;
}

static int write_iwi_rgba(const char *path) { return write_iwi_components(path, 4); }
static int write_iwi_rgb(const char *path)  { return write_iwi_components(path, 3); }

static int write_png(const char *path)
{
    uint8_t rgba[W * H * 4];
    fill_rgba(rgba);

    FILE *f = fopen(path, "wb");
    if (!f) return 1;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return 1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(f); return 1; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return 1;
    }

    png_init_io(png, f);
    /* Explicit sRGB chunk: the loader's colour-space rule prefers an explicit
     * tag over its bit-depth heuristic, and the test asserts SRGB. */
    png_set_IHDR(png, info, W, H, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    png_write_info(png, info);
    for (int y = 0; y < H; ++y) {
        png_bytep row = rgba + (size_t)y * W * 4;
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return 0;
}

static int write_webp(const char *path)
{
    uint8_t rgba[W * H * 4];
    fill_rgba(rgba);

    uint8_t *out = NULL;
    size_t n = WebPEncodeLosslessRGBA(rgba, W, H, W * 4, &out);
    if (n == 0 || !out) return 1;

    FILE *f = fopen(path, "wb");
    if (!f) { WebPFree(out); return 1; }
    const size_t written = fwrite(out, 1, n, f);
    fclose(f);
    WebPFree(out);
    return written == n ? 0 : 1;
}

static int write_tiff(const char *path)
{
    uint8_t rgb[W * H * 3];
    fill_rgb(rgb);

    TIFF *tif = TIFFOpen(path, "w");
    if (!tif) return 1;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)W);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)H);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)H);

    for (int y = 0; y < H; ++y) {
        if (TIFFWriteScanline(tif, rgb + (size_t)y * W * 3, (uint32_t)y, 0) < 0) {
            TIFFClose(tif);
            return 1;
        }
    }
    TIFFClose(tif);
    return 0;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int write_bmp(const char *path)
{
    uint8_t rgb[W * H * 3];
    fill_rgb(rgb);

    /* BMP rows are bottom-up and padded to a 4-byte boundary.  4 pixels at
     * 24 bpp is 12 bytes, already a multiple of 4. */
    const uint32_t rowBytes = W * 3u;
    const uint32_t pixelBytes = rowBytes * (uint32_t)H;
    const uint32_t fileBytes = 54u + pixelBytes;

    uint8_t header[54];
    memset(header, 0, sizeof header);
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, fileBytes);
    put_u32(header + 10, 54u);       /* pixel data offset */
    put_u32(header + 14, 40u);       /* BITMAPINFOHEADER size */
    put_u32(header + 18, (uint32_t)W);
    put_u32(header + 22, (uint32_t)H); /* positive: bottom-up */
    put_u16(header + 26, 1);         /* planes */
    put_u16(header + 28, 24);        /* bits per pixel */
    put_u32(header + 30, 0);         /* BI_RGB */
    put_u32(header + 34, pixelBytes);

    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    if (fwrite(header, 1, sizeof header, f) != sizeof header) { fclose(f); return 1; }

    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t *p = rgb + ((size_t)y * W + x) * 3;
            uint8_t bgr[3] = { p[2], p[1], p[0] };
            if (fwrite(bgr, 1, 3, f) != 3) { fclose(f); return 1; }
        }
    }
    fclose(f);
    return 0;
}

/*
 * GIF (GIF89a), palettized.
 *
 * The palette is the 16 distinct colours of the shared 4x4 pattern in raster
 * order, so palette index i is pixel i and RasterBits is just 0,1,2,...,15.
 *
 * LZW: this encoder deliberately does not compress.  It emits a CLEAR code
 * before every literal, which resets the decoder's dictionary each time, so
 * the dictionary never grows past its initial size and the code width never
 * changes from 5 bits.  That removes the one thing that reliably goes wrong in
 * a hand-written GIF encoder -- the encoder and decoder disagreeing about the
 * exact code at which the width grows -- at a cost of about 10 extra bytes in
 * a 100-byte fixture.  Compression here would buy nothing and risk a fixture
 * that only some decoders accept.
 */
static int write_gif(const char *path)
{
    uint8_t rgb[W * H * 3];
    fill_rgb(rgb);

    /* Palette in raster order; index i == pixel i. */
    uint8_t palette[16 * 3];
    for (int i = 0; i < W * H; ++i)
        memcpy(palette + i * 3, rgb + (size_t)i * 3, 3);

    uint8_t out[256];
    size_t n = 0;

    /* Header */
    memcpy(out + n, "GIF89a", 6); n += 6;

    /* Logical Screen Descriptor.
     *   packed: GCT present | colour resolution 4 bits | sort 0 | GCT size 16
     *   GCT size field is log2(entries) - 1, so 4 entries -> ... and 16 -> 3. */
    put_u16(out + n, (uint16_t)W); n += 2;
    put_u16(out + n, (uint16_t)H); n += 2;
    out[n++] = 0x80u | 0x30u | 0x03u;
    out[n++] = 0;    /* background colour index */
    out[n++] = 0;    /* pixel aspect ratio: none */

    /* Global Colour Table */
    memcpy(out + n, palette, sizeof palette); n += sizeof palette;

    /* Image Descriptor: no local colour table, not interlaced. */
    out[n++] = 0x2Cu;
    put_u16(out + n, 0); n += 2;         /* left */
    put_u16(out + n, 0); n += 2;         /* top */
    put_u16(out + n, (uint16_t)W); n += 2;
    put_u16(out + n, (uint16_t)H); n += 2;
    out[n++] = 0x00u;

    /* LZW minimum code size: 4 bits per index.
     * clear = 16, end-of-information = 17, codes are 5 bits wide. */
    out[n++] = 4;

    /* Bit-pack: CLEAR, literal, CLEAR, literal, ... , EOI.  Least significant
     * bit first, which is GIF's packing order. */
    uint8_t  block[64];
    size_t   bitCount = 0;
    size_t   bytes    = 0;
    uint32_t acc      = 0;

    for (int i = 0; i < W * H; ++i) {
        for (int c = 0; c < 2; ++c) {
            const uint32_t code = (c == 0) ? 16u : (uint32_t)i;  /* CLEAR, then literal */
            acc |= code << bitCount;
            bitCount += 5;
            while (bitCount >= 8) {
                block[bytes++] = (uint8_t)(acc & 0xFFu);
                acc >>= 8;
                bitCount -= 8;
            }
        }
    }
    /* End of information */
    acc |= 17u << bitCount;
    bitCount += 5;
    while (bitCount > 0) {
        block[bytes++] = (uint8_t)(acc & 0xFFu);
        acc >>= 8;
        bitCount = (bitCount >= 8) ? bitCount - 8 : 0;
    }

    /* Data sub-blocks: one length byte followed by that many bytes. */
    size_t off = 0;
    while (off < bytes) {
        const size_t chunk = (bytes - off > 255) ? 255 : bytes - off;
        out[n++] = (uint8_t)chunk;
        memcpy(out + n, block + off, chunk);
        n += chunk;
        off += chunk;
    }
    out[n++] = 0;    /* block terminator */

    out[n++] = 0x3Bu; /* trailer */

    if (n > sizeof out) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    const size_t written = fwrite(out, 1, n, f);
    fclose(f);
    return written == n ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }

    char path[4096];
    struct { const char *name; int (*write)(const char *); } jobs[] = {
        { "rgba_4x4.png",            write_png },
        { "lossless_4x4.webp",       write_webp },
        { "rgb_4x4.tif",             write_tiff },
        { "rgb_4x4.bmp",             write_bmp },
        { "gif_4x4.gif",             write_gif },
        { "rgba_4x4.iwi",            write_iwi_rgba },
        { "rgb_4x4.iwi",             write_iwi_rgb },
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof jobs / sizeof jobs[0]; ++i) {
        snprintf(path, sizeof path, "%s/%s", argv[1], jobs[i].name);
        if (jobs[i].write(path) != 0) {
            fprintf(stderr, "failed: %s\n", path);
            ++failures;
        } else {
            printf("wrote %s\n", path);
        }
    }
    return failures == 0 ? 0 : 1;
}
