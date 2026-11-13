/*
 * iwi_file_c_test.c — proves the IWI container API is really a C API.
 *
 * Everything else about this format is tested from C++ (doctest suites, module
 * unit tests).  That is exactly the blind spot this file covers: a header can
 * be called "C" while depending on C++ constructs that only compile when some
 * other translation unit has already dragged in <cstdint> or `extern "C"`.
 *
 * So this is compiled as C99 against the public header, linked against the
 * library as a C consumer would link it, and it exercises the API end to end:
 * a valid file, each refusal class, and the two helper functions.  If someone
 * later moves the implementation into C++ and forgets `extern "C"`, or adds a
 * declaration that needs C++, this stops compiling.
 *
 * It is also the shape a game engine would consume: no C++, no modules, no
 * allocation, one header and one static library.
 */

#include <stdio.h>
#include <string.h>

#include "IwiFile.h"

static int failures = 0;

static void check(int condition, const char* what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

/* Build a minimal container: 74-byte header + tightly packed payload. */
static size_t make_iwi(unsigned char* buf, size_t bufSize,
                       uint32_t width, uint32_t height, uint16_t components)
{
    const size_t payload = (size_t)width * (size_t)height * (size_t)components;
    size_t i;

    if (bufSize < IWI_FILE_HEADER_BYTES + payload)
        return 0;

    memset(buf, 0, IWI_FILE_HEADER_BYTES + payload);
    memcpy(buf, "IWI1", 4);

    /* width @4, height @8, little-endian. */
    buf[4]  = (unsigned char)(width & 0xFFu);
    buf[5]  = (unsigned char)((width >> 8) & 0xFFu);
    buf[6]  = (unsigned char)((width >> 16) & 0xFFu);
    buf[7]  = (unsigned char)((width >> 24) & 0xFFu);
    buf[8]  = (unsigned char)(height & 0xFFu);
    buf[9]  = (unsigned char)((height >> 8) & 0xFFu);
    buf[10] = (unsigned char)((height >> 16) & 0xFFu);
    buf[11] = (unsigned char)((height >> 24) & 0xFFu);

    buf[12] = 0;                                   /* sRGB */
    buf[13] = (unsigned char)(components & 0xFFu); /* componentCount */
    buf[14] = (unsigned char)((components >> 8) & 0xFFu);
    buf[15] = (unsigned char)((components * 8u) & 0xFFu);
    buf[16] = 0;
    buf[17] = 0;                                   /* interleaved */

    for (i = 0; i < (size_t)components; ++i) {
        const size_t at = 18u + i * (size_t)IWI_FILE_COMPONENT_BYTES;
        buf[at + 0] = (unsigned char)(i + 1u);     /* R, G, B[, A] */
        buf[at + 1] = (unsigned char)IW_COMPONENT_CLASS_UNORM;
        buf[at + 2] = (unsigned char)(i * 8u);     /* bitOffset lo */
        buf[at + 3] = 0;
        buf[at + 4] = 8;                           /* bitWidth lo */
        buf[at + 5] = 0;
    }

    /* pixelDataSize @66, little-endian u64. */
    buf[66] = (unsigned char)(payload & 0xFFu);
    buf[67] = (unsigned char)((payload >> 8) & 0xFFu);
    buf[68] = (unsigned char)((payload >> 16) & 0xFFu);
    buf[69] = (unsigned char)((payload >> 24) & 0xFFu);

    for (i = 0; i < payload; ++i)
        buf[IWI_FILE_HEADER_BYTES + i] = (unsigned char)0x5A;

    return IWI_FILE_HEADER_BYTES + payload;
}

int main(void)
{
    unsigned char buf[512];
    IwiFileHeader h;
    char reason[192];
    size_t size;

    /* --- A valid 2x2 RGBA container ---------------------------------- */
    size = make_iwi(buf, sizeof(buf), 2, 2, 4);
    check(size > 0, "fixture built");

    check(iwi_file_parse_header(buf, size, &h, reason, sizeof(reason))
          == IWI_FILE_OK, "valid RGBA parses");
    check(h.width == 2 && h.height == 2, "geometry");
    check(h.componentCount == 4, "component count");
    check(h.bitsPerPixel == 32, "bits per pixel");
    check(h.storageLayout == IW_STORAGE_INTERLEAVED, "storage layout");
    check(h.packedBytes == 16, "packed bytes");
    check(h.pixelBytes == 16, "declared bytes");
    check(h.components[0].semantic == IW_COMPONENT_SEMANTIC_R, "semantic R");
    check(h.components[3].bitOffset == 24, "alpha at bit 24");

    /* The helper agrees with the parse. */
    check(iwi_file_required_bytes(2, 2, 4) == 16, "required bytes");
    check(iwi_file_required_bytes(0, 0, 4) == 0, "zero geometry");

    /* --- A valid 2x2 RGB container ----------------------------------- */
    size = make_iwi(buf, sizeof(buf), 2, 2, 3);
    check(iwi_file_parse_header(buf, size, &h, reason, sizeof(reason))
          == IWI_FILE_OK, "valid RGB parses");
    check(h.components[3].semantic == 0, "fourth slot is zeroed, not invented");

    /* --- Refusals, each one a distinct status ------------------------ */
    {
        unsigned char bad[512];

        /* Wrong magic. */
        memcpy(bad, buf, size);
        bad[0] = 'P';
        check(iwi_file_parse_header(bad, size, &h, reason, sizeof(reason))
              == IWI_FILE_ERR_BAD_MAGIC, "bad magic");
        check(strstr(reason, "magic") != NULL, "bad magic reason names it");

        /* Short header. */
        check(iwi_file_parse_header(bad, 10, &h, reason, sizeof(reason))
              == IWI_FILE_ERR_TRUNCATED_HEADER, "truncated header");
        check(strstr(reason, "header") != NULL, "truncated header reason");

        /* Planar storage. */
        memcpy(bad, buf, size);
        bad[17] = 1;
        check(iwi_file_parse_header(bad, size, &h, reason, sizeof(reason))
              == IWI_FILE_ERR_PLANAR, "planar refused");

        /* Components out of R,G,B order. */
        memcpy(bad, buf, size);
        bad[18] = IW_COMPONENT_SEMANTIC_B;
        check(iwi_file_parse_header(bad, size, &h, reason, sizeof(reason))
              == IWI_FILE_ERR_COMPONENT_ORDER, "wrong semantic refused");

        /* Payload one byte short of what the header declares. */
        memcpy(bad, buf, size);
        check(iwi_file_parse_header(bad, size - 1, &h, reason, sizeof(reason))
              == IWI_FILE_ERR_PAYLOAD_TRUNCATED, "short payload refused");

        /* A null buffer is a status, not a crash. */
        check(iwi_file_parse_header(NULL, 0, &h, reason, sizeof(reason))
              == IWI_FILE_ERR_NO_INPUT, "null input");
    }

    /* --- The helper never returns a bogus size on overflow ----------- */
    check(iwi_file_required_bytes(0xFFFFFFFFu, 0xFFFFFFFFu, 8) == 0,
          "overflow returns 0");

    /* --- Status names are never NULL --------------------------------- */
    check(iwi_file_status_name(IWI_FILE_OK) != NULL, "ok has a name");
    check(iwi_file_status_name(IWI_FILE_ERR_PLANAR) != NULL, "planar has a name");
    check(strcmp(iwi_file_status_name(IWI_FILE_ERR_BAD_MAGIC), "bad-magic") == 0,
          "bad magic name");

    /* --- A caller argument may be a null out-pointer ------------------ */
    {
        size_t realSize = make_iwi(buf, sizeof(buf), 2, 2, 4);
        check(iwi_file_parse_header(buf, realSize, NULL, NULL, 0) == IWI_FILE_OK,
              "validation without an out-parameter");
    }

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("iwi_file_c_test: all checks passed\n");
    return 0;
}
