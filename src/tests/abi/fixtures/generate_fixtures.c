/*
 * generate_fixtures.c — regenerates the JPEG fixtures in this directory.
 *
 * The four .jpg files here are 16x16 synthetic images whose only job is to give
 * the JPEG loader ABI tests a known component count.  They are checked in as
 * binaries, so this file exists to make them reproducible rather than opaque:
 * rebuilding from this source reproduces all four byte-for-byte.
 *
 *   cc -O2 -o /tmp/gen_fixtures generate_fixtures.c -ljpeg
 *   mkdir -p /tmp/fx && /tmp/gen_fixtures /tmp/fx
 *   for f in gray_1comp ycbcr_3comp cmyk_4comp two_comp; do
 *     cmp /tmp/fx/$f.jpg $f.jpg && echo "ok $f"
 *   done
 *
 * Reproduced byte-identically with libjpeg-turbo 8 (JPEG_LIB_VERSION 80) on
 * Linux x86-64.  A different libjpeg major version may encode the entropy-
 * coded segment differently while producing a structurally identical file;
 * that is harmless, the tests only depend on the component count.
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
 *   gray_1comp.jpg    1 component,  128  — plain grayscale
 *   ycbcr_3comp.jpg   3 components, 128  — YCbCr, mid-grey after conversion
 *   cmyk_4comp.jpg    4 components, 255  — Adobe-style CMYK with the APP14
 *                                          marker.  libjpeg writes CMYK samples
 *                                          inverted (Adobe convention), so 255
 *                                          means "no ink", i.e. white, not
 *                                          mid-grey despite what a uniform
 *                                          value might suggest
 *   two_comp.jpg      2 components, 128  — no standard 2-component colour space
 *                                          exists, hence JCS_UNKNOWN.  This is
 *                                          a synthetic file: it exists only so
 *                                          the loader's "reject 2 components"
 *                                          path has something to reject
 */

#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>

#define DIM 16

static void emit(const char* path, int components, int value)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE* out = fopen(path, "wb");
    if (out == NULL) {
        perror(path);
        exit(EXIT_FAILURE);
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, out);

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
    fclose(out);
    jpeg_destroy_compress(&cinfo);
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
    return EXIT_SUCCESS;
}
