/*
 * generate_fixtures.c — regenerates the JPEG fixtures in this directory.
 *
 * The .jpg files here are synthetic images whose job is to give the JPEG
 * loader ABI tests known properties.  They are checked in as binaries, so this
 * file exists to make them reproducible rather than opaque: rebuilding from
 * this source reproduces them byte-for-byte.
 *
 *   cc -O2 -o /tmp/gen_fixtures generate_fixtures.c -ljpeg
 *   mkdir -p /tmp/fx && /tmp/gen_fixtures /tmp/fx
 *   for f in gray_1comp ycbcr_3comp cmyk_4comp two_comp orient6_3comp; do
 *     cmp /tmp/fx/$f.jpg $f.jpg && echo "ok $f"
 *   done
 *
 * Reproduced byte-identically with libjpeg-turbo 8 (JPEG_LIB_VERSION 80) on
 * Linux x86-64.  A different libjpeg major version may encode the entropy-
 * coded segment differently while producing a structurally identical file;
 * that is harmless, the tests only depend on the component count and, for
 * orient6_3comp.jpg, on the EXIF tag and the reconstructed flat halves.
 *
 * Parameters that matter, and why they are not the library defaults:
 *
 *   quality 95        — matches the quantisation tables of the checked-in files
 *   h/v_samp 1x1      — libjpeg otherwise defaults 3-component input to 4:2:0
 *                       (2x2 luma), which the checked-in files do not use
 *   CMYK ids "CMYK"   — ASCII component ids, as written by ImageMagick.  The
 *                       defaults would use numeric ids and the file would not
 *                       match the checked-in one
 *   in_color_space    — JCS_UNKNOWN for the 2- and 4-component cases is what
 *                       suppresses the JFIF APP0 header, matching the files
 *
 * Component counts and sample values:
 *
 *   gray_1comp.jpg    1 component,  128  — 16x16 plain grayscale
 *   ycbcr_3comp.jpg   3 components, 128  — 16x16 YCbCr, mid-grey after conversion
 *   cmyk_4comp.jpg    4 components, 255  — 16x16 Adobe-style CMYK with the APP14
 *                                          marker.  libjpeg writes CMYK samples
 *                                          inverted (Adobe convention), so 255
 *                                          means "no ink", i.e. white, not
 *                                          mid-grey despite what a uniform
 *                                          value might suggest
 *   two_comp.jpg      2 components, 128  — 16x16.  No standard 2-component
 *                                          colour space exists, hence JCS_UNKNOWN.
 *                                          This is a synthetic file: it exists
 *                                          only so the loader's "reject 2
 *                                          components" path has something to
 *                                          reject
 *   orient6_3comp.jpg 3 components       — 16x8, left half white and right half
 *                                          black, with an EXIF APP1 segment
 *                                          declaring Orientation=6 (rotate 90°
 *                                          clockwise).  The raster is stored
 *                                          landscape, exactly as a phone camera
 *                                          writes a portrait photo; a loader
 *                                          that honours the tag returns 8x16
 *                                          with the white half on top
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define DIM 16

/*
 * Encodes to memory and writes the buffer in one go rather than handing a
 * FILE* to jpeg_stdio_dest.  Observed on Windows: passing the executable's
 * FILE* into the vcpkg libjpeg-turbo DLL makes jpeg_finish_compress fastfail
 * inside the CRT's fflush (ucrtbase, 0xC0000409).  The memory form is also
 * what the APP1 splice needs, and it keeps the encoded bytes independent of
 * how the host CRT buffers files.
 */
static void emit(const char* path, int components, int value)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char* jpeg = NULL;
    unsigned long jpeg_size = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &jpeg, &jpeg_size);

    cinfo.image_width      = DIM;
    cinfo.image_height     = DIM;
    cinfo.input_components = components;
    cinfo.in_color_space   = (components == 1) ? JCS_GRAYSCALE
                           : (components == 3) ? JCS_RGB
                           : (components == 4) ? JCS_CMYK
                           : JCS_UNKNOWN;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 95, TRUE);

    for (int i = 0; i < components; i++) {
        cinfo.comp_info[i].h_samp_factor = 1;
        cinfo.comp_info[i].v_samp_factor = 1;
        if (components == 4) {
            cinfo.comp_info[i].component_id = "CMYK"[i];
        }
    }

    jpeg_start_compress(&cinfo, TRUE);

    unsigned char row[DIM * 4];
    for (int i = 0; i < DIM * components; i++) {
        row[i] = (unsigned char)value;
    }
    JSAMPROW rows[1] = { row };
    while (cinfo.next_scanline < cinfo.image_height) {
        jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    FILE* out = fopen(path, "wb");
    if (out == NULL) {
        perror(path);
        free(jpeg);
        exit(EXIT_FAILURE);
    }
    fwrite(jpeg, 1, (size_t)jpeg_size, out);
    fclose(out);
    free(jpeg);
}

/*
 * Encodes a 16x8 two-tone raster and inserts an EXIF APP1 segment carrying
 * Orientation=orientation.  The pixels stay in sensor order and only the tag
 * says how to turn them — exactly the situation a portrait phone photo
 * presents to a loader.
 *
 * The written stream is: SOI, APP1 ("Exif\0\0" + TIFF with tag 0x0112), then
 * the rest of the encoded JPEG.  EXIF wants APP1 immediately after SOI, so it
 * goes ahead of libjpeg's JFIF APP0.
 */
static void emit_oriented(const char* path, int orientation)
{
    const int width  = 16;
    const int height = 8;

    unsigned char* jpeg = NULL;
    unsigned long jpeg_size = 0;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &jpeg, &jpeg_size);

    cinfo.image_width      = width;
    cinfo.image_height     = height;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 95, TRUE);
    for (int i = 0; i < 3; i++) {
        cinfo.comp_info[i].h_samp_factor = 1;
        cinfo.comp_info[i].v_samp_factor = 1;
    }

    jpeg_start_compress(&cinfo, TRUE);

    /* Left half white, right half black.  Each half covers whole 8x8 DCT
     * blocks, so the reconstruction stays flat and the ABI test can hold it to
     * a tight tolerance. */
    unsigned char row[16 * 3];
    for (int x = 0; x < width; x++) {
        const unsigned char v = (x < width / 2) ? 255 : 0;
        row[x * 3 + 0] = v;
        row[x * 3 + 1] = v;
        row[x * 3 + 2] = v;
    }
    JSAMPROW rows[1] = { row };
    while (cinfo.next_scanline < cinfo.image_height) {
        jpeg_write_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    /* TIFF block: big-endian ("MM"), IFD0 at offset 8 with a single entry. */
    unsigned char tiff[26] = { 0 };
    tiff[0]  = 'M';  tiff[1]  = 'M';
    tiff[2]  = 0x00; tiff[3]  = 0x2A;              /* magic 42 */
    tiff[4]  = 0x00; tiff[5]  = 0x00;              /* IFD0 at offset 8 */
    tiff[6]  = 0x00; tiff[7]  = 0x08;
    tiff[8]  = 0x00; tiff[9]  = 0x01;              /* one entry */
    tiff[10] = 0x01; tiff[11] = 0x12;              /* tag 0x0112, Orientation */
    tiff[12] = 0x00; tiff[13] = 0x03;              /* type SHORT */
    tiff[14] = 0x00; tiff[15] = 0x00;              /* count 1 */
    tiff[16] = 0x00; tiff[17] = 0x01;
    tiff[18] = 0x00; tiff[19] = (unsigned char)orientation;
    /* tiff[20..21] value padding, tiff[22..25] next-IFD offset = 0 */

    /* APP1 payload is "Exif\0\0" + the TIFF block; the 2-byte length field
     * counts itself but not the marker byte pair. */
    unsigned char app1[4 + 6 + sizeof tiff];
    const unsigned payload = 2 + 6 + (unsigned)sizeof tiff;
    app1[0] = 0xFF; app1[1] = 0xE1;
    app1[2] = (unsigned char)(payload >> 8);
    app1[3] = (unsigned char)(payload & 0xFF);
    memcpy(app1 + 4, "Exif\0\0", 6);
    memcpy(app1 + 10, tiff, sizeof tiff);

    FILE* out = fopen(path, "wb");
    if (out == NULL) {
        perror(path);
        free(jpeg);
        exit(EXIT_FAILURE);
    }
    fwrite(jpeg, 1, 2, out);                        /* SOI */
    fwrite(app1, 1, sizeof app1, out);              /* EXIF APP1 */
    fwrite(jpeg + 2, 1, (size_t)(jpeg_size - 2), out);
    fclose(out);
    free(jpeg);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char path[1024];
    struct { const char* name; int components; int value; } specs[] = {
        { "gray_1comp.jpg",  1, 128 },
        { "ycbcr_3comp.jpg", 3, 128 },
        { "cmyk_4comp.jpg",  4, 255 },
        { "two_comp.jpg",    2, 128 },
    };

    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", argv[1], specs[i].name);
        emit(path, specs[i].components, specs[i].value);
        printf("%s\n", path);
    }

    snprintf(path, sizeof path, "%s/orient6_3comp.jpg", argv[1]);
    emit_oriented(path, 6);
    printf("%s\n", path);

    return EXIT_SUCCESS;
}
