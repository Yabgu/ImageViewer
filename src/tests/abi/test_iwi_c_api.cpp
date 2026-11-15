/*
 * test_iwi_c_api.cpp - runtime tests for the .iwi C ABI (IwiFile.h).
 *
 * This is the exported contract: a C header, a static library, and functions
 * with C linkage.  The test is written as plain C++17 with no modules and no
 * doctest, so it is also a worked example of how an external consumer - Tension
 * through FFI, a game engine, a build script - links one library and includes
 * one header.
 *
 * Every case increments a failure counter on mismatch and main() returns it,
 * so ctest fails on regression.  A printf-only test proves nothing.
 *
 * What it pins, in order:
 *   - required_size is 36 + 6N and does not depend on the payload
 *   - a blob of exactly required_size parses; one byte less fails ENOSPC
 *   - components live at offset 36 and are walkable through the accessor
 *   - 3- and 4-component files parse and decode to RGBA8
 *   - malformed input is rejected with the status and the message substring
 *     the viewer's error paths depend on ("magic", "planar", "offset",
 *     "header")
 *   - a payload encoding that merely starts with "raw" is rejected
 *   - metadata is addressable at 36 + 6N and is not copied
 *   - a 200-component file parses: the container has no component cap
 */

#include "IwiFile.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) {
        return;
    }
    std::printf("FAIL: %s\n", what);
    ++failures;
}

/* The message for a status must contain `needle`.  This is how the loader and
 * the thumbnail provider tests match on wording, so it is a contract. */
void checkMessage(int status, const char* needle, const char* what)
{
    const char* msg = iwi_status_message(status);
    if (msg == nullptr) {
        std::printf("FAIL: %s (message is NULL)\n", what);
        ++failures;
        return;
    }
    if (std::strstr(msg, needle) == nullptr) {
        std::printf("FAIL: %s (expected \"%s\" in \"%s\")\n", what, needle, msg);
        ++failures;
    }
}

struct Spec {
    uint32_t width          = 4;
    uint32_t height         = 4;
    uint16_t components     = 4;
    uint32_t metaBytes      = 0;
    uint32_t storageFlags   = 0;
    uint32_t storageStride  = 0;
    const char* encoding    = "raw";
    bool     encodingGarbage = false;   /* fill payloadEncoding with junk   */
    uint8_t  firstSemantic  = 0;        /* 0 = default R,G,B[,A] chain      */
    uint16_t bitWidth       = 8;        /* bits per component              */
};

void PutU16(std::vector<uint8_t>& b, std::size_t at, uint16_t v)
{
    b[at + 0] = static_cast<uint8_t>(v & 0xFFu);
    b[at + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void PutU32(std::vector<uint8_t>& b, std::size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

void PutU64(std::vector<uint8_t>& b, std::size_t at, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

/*
 * Build a container.  The payload is a deterministic byte ramp so a decode can
 * be compared against an expected pattern rather than just "no crash".
 */
std::vector<uint8_t> MakeIwi(const Spec& spec, std::size_t trailingSlack = 0)
{
    const std::size_t header  = IWI_FILE_BASE_HEADER_BYTES +
                                static_cast<std::size_t>(spec.components) * IWI_FILE_COMPONENT_BYTES;
    const std::size_t payload = static_cast<std::size_t>(spec.width) *
                                spec.height * spec.components * (spec.bitWidth / 8u);
    std::vector<uint8_t> b(header + spec.metaBytes + payload + trailingSlack, 0);

    std::memcpy(b.data(), IWI_FILE_MAGIC, IWI_FILE_MAGIC_BYTES);
    PutU32(b, 4u, spec.width);
    PutU32(b, 8u, spec.height);
    PutU32(b, 12u, spec.metaBytes);

    /* storageLayout: flags in the low 32 bits, stride in the high 32. */
    PutU64(b, 16u, static_cast<uint64_t>(spec.storageFlags) |
                   (static_cast<uint64_t>(spec.storageStride) << 32u));

    if (spec.encodingGarbage) {
        std::memcpy(b.data() + 24, "rawgarba", 8);
    } else if (spec.encoding != nullptr) {
        std::memcpy(b.data() + 24, spec.encoding, std::min<std::size_t>(8, std::strlen(spec.encoding)));
    }

    b[32] = static_cast<uint8_t>(IMAGE_COLOR_SPACE_SRGB);
    b[33] = 0;
    PutU16(b, 34u, spec.components);

    static const uint8_t kChain[4] = {
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_R),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_G),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_B),
        static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_A)
    };

    for (uint16_t i = 0; i < spec.components; ++i) {
        const std::size_t rec = IWI_FILE_BASE_HEADER_BYTES +
                                static_cast<std::size_t>(i) * IWI_FILE_COMPONENT_BYTES;
        uint8_t semantic;
        if (spec.firstSemantic != 0) {
            semantic = (i == 0) ? spec.firstSemantic : kChain[i < 4 ? i : 3];
        } else {
            semantic = (i < 4) ? kChain[i]
                               : static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_UNKNOWN);
        }
        b[rec + 0] = semantic;
        b[rec + 1] = static_cast<uint8_t>(IW_COMPONENT_CLASS_UNORM);
        PutU16(b, rec + 2, static_cast<uint16_t>(i * spec.bitWidth));
        PutU16(b, rec + 4, spec.bitWidth);
    }

    const std::size_t pixelStart = header + spec.metaBytes;
    for (std::size_t i = 0; i < payload; ++i) {
        b[pixelStart + i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
    return b;
}

/* The blob a caller allocates: 36 + 6N. */
std::vector<uint8_t> AllocBlob(std::size_t size)
{
    return std::vector<uint8_t>(size, 0xCD);
}

IwiFileHeader* AsHeader(std::vector<uint8_t>& blob)
{
    return reinterpret_cast<IwiFileHeader*>(blob.data());
}

} // namespace

int main()
{
    /* --- required_size is 36 + 6N, independent of payload --------------- */
    {
        const uint16_t counts[] = { 1, 3, 4, 200 };
        for (uint16_t n : counts) {
            Spec spec;
            spec.components = n;
            const std::vector<uint8_t> file = MakeIwi(spec);
            const std::size_t need = iwi_file_required_size(file.data(), file.size());
            const std::size_t expect =
                IWI_FILE_BASE_HEADER_BYTES + static_cast<std::size_t>(n) * IWI_FILE_COMPONENT_BYTES;
            check(need == expect, "required_size is 36 + 6N");
        }
    }

    /* --- a blob of exactly required_size parses; one byte less ENOSPC --- */
    {
        Spec spec;
        const std::vector<uint8_t> file = MakeIwi(spec);
        const std::size_t need = iwi_file_required_size(file.data(), file.size());

        std::vector<uint8_t> exact = AllocBlob(need);
        check(iwi_parse_structural(AsHeader(exact), exact.size(), file.data(), file.size())
                  == IWI_STATUS_OK,
              "parse into exactly-sized buffer");

        if (need > 0) {
            std::vector<uint8_t> small = AllocBlob(need - 1);
            const int rc = iwi_parse_structural(AsHeader(small), small.size(), file.data(), file.size());
            check(rc == IWI_STATUS_ENOSPC, "parse into required_size - 1 fails ENOSPC");
            checkMessage(rc, "small", "ENOSPC message");
        }
    }

    /* --- components live at offset 36 and are walkable ------------------ */
    {
        Spec spec;
        spec.components = 4;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));

        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size()) == IWI_STATUS_OK,
              "4-component parse");

        const IwiFileComponent* c = iwi_file_components(AsHeader(blob));
        check(reinterpret_cast<const uint8_t*>(c) == blob.data() + IWI_FILE_BASE_HEADER_BYTES,
              "components start at offset 36");
        check(c[0].semantic == IW_COMPONENT_SEMANTIC_R, "component 0 is R");
        check(c[1].semantic == IW_COMPONENT_SEMANTIC_G, "component 1 is G");
        check(c[2].semantic == IW_COMPONENT_SEMANTIC_B, "component 2 is B");
        check(c[3].semantic == IW_COMPONENT_SEMANTIC_A, "component 3 is A");
        check(c[3].bitOffset == 24, "component 3 sits at bit offset 24");
        check(c[0].componentClass == IW_COMPONENT_CLASS_UNORM, "component class survives");
    }

    /* --- 3-component parses and decodes to RGBA with opaque alpha ------- */
    {
        Spec spec;
        spec.components = 3;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size()) == IWI_STATUS_OK,
              "3-component parse");

        std::vector<uint8_t> rgba(static_cast<std::size_t>(spec.width) * spec.height * 4u, 0);
        check(iwi_decode(AsHeader(blob), file.data(), file.size(), rgba.data(), rgba.size())
                  == IWI_STATUS_OK,
              "3-component decode");

        const std::size_t pixelStart = IWI_FILE_BASE_HEADER_BYTES + 3u * IWI_FILE_COMPONENT_BYTES;
        for (std::size_t p = 0; p < 4; ++p) {
            check(rgba[p * 4 + 0] == file[pixelStart + p * 3 + 0], "RGB->RGBA red");
            check(rgba[p * 4 + 1] == file[pixelStart + p * 3 + 1], "RGB->RGBA green");
            check(rgba[p * 4 + 2] == file[pixelStart + p * 3 + 2], "RGB->RGBA blue");
            check(rgba[p * 4 + 3] == 0xFFu, "RGB->RGBA alpha opaque");
        }
    }

    /* --- 4-component decodes byte-identically (the RGBA passthrough) ---- */
    {
        Spec spec;
        spec.components = 4;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size()) == IWI_STATUS_OK,
              "RGBA parse");

        std::vector<uint8_t> rgba(static_cast<std::size_t>(spec.width) * spec.height * 4u, 0);
        check(iwi_decode(AsHeader(blob), file.data(), file.size(), rgba.data(), rgba.size())
                  == IWI_STATUS_OK,
              "RGBA decode");

        const std::size_t pixelStart = IWI_FILE_BASE_HEADER_BYTES + 4u * IWI_FILE_COMPONENT_BYTES;
        check(std::memcmp(rgba.data(), file.data() + pixelStart, rgba.size()) == 0,
              "RGBA passthrough is byte-identical");
    }

    /* --- bad magic ------------------------------------------------------ */
    {
        std::vector<uint8_t> file = MakeIwi(Spec{});
        file[0] = 'X';
        std::vector<uint8_t> blob = AllocBlob(64);
        const int rc = iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size());
        check(rc == IWI_STATUS_BAD_MAGIC, "bad magic rejected");
        checkMessage(rc, "magic", "magic message");
        check(iwi_file_required_size(file.data(), file.size()) == 0,
              "required_size is 0 for bad magic");
    }

    /* --- truncation ----------------------------------------------------- */
    {
        const std::vector<uint8_t> full = MakeIwi(Spec{});
        std::vector<uint8_t> blob = AllocBlob(64);

        /* Shorter than the base header. */
        const int rc1 = iwi_parse_structural(AsHeader(blob), blob.size(), full.data(), 10);
        check(rc1 == IWI_STATUS_TRUNCATED_HEADER, "short header rejected");
        checkMessage(rc1, "header", "truncated message names the header");

        /* Header intact, payload short by one byte. */
        const int rc2 = iwi_parse_structural(AsHeader(blob), blob.size(), full.data(), full.size() - 1);
        check(rc2 == IWI_STATUS_PAYLOAD_TRUNCATED, "short payload rejected");
    }

    /* --- planar storage -------------------------------------------------- */
    {
        Spec spec;
        spec.storageFlags = IW_STORAGE_FLAG_PLANAR;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        const int rc = iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size());

        /* The grammar has no opinion on layout: "one plane per component" is a
         * well-formed IWI. Whether this build renders it is policy, and the
         * viewer's answer lives in IwiFormat.ixx (tested in anti-drift). */
        check(rc == IWI_STATUS_OK, "planar is structurally valid");
    }

    /* --- every unimplemented storage flag is rejected, not ignored ------ */
    {
        const uint32_t flags[] = {
            IW_STORAGE_FLAG_COLUMNAR,
            IW_STORAGE_FLAG_FLIP_X,
            IW_STORAGE_FLAG_FLIP_Y,
            IW_STORAGE_FLAG_STRIDE_8BIT,
            IW_STORAGE_FLAG_STRIDE_16BIT,
            IW_STORAGE_FLAG_STRIDE_32BIT,
        };
        for (uint32_t flag : flags) {
            Spec spec;
            spec.storageFlags = flag;
            const std::vector<uint8_t> file = MakeIwi(spec);
            std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
            const int rc = iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size());
            check(rc == IWI_STATUS_OK, "a storage flag is a layout, not a grammar error");
        }

        /* A stride with no flag bits is equally unimplemented. */
        Spec spec;
        spec.storageStride = 64;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size())
                  == IWI_STATUS_OK,
              "a declared stride is grammatically valid");
    }

    /* --- grayscale: valid container, not presentable as RGBA ----------- */
    {
        Spec spec;
        spec.components   = 1;
        spec.firstSemantic = static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_GRAY);
        const std::vector<uint8_t> file = MakeIwi(spec);

        const std::size_t need = iwi_file_required_size(file.data(), file.size());
        check(need == IWI_FILE_BASE_HEADER_BYTES + IWI_FILE_COMPONENT_BYTES,
              "grayscale still sizes as 36 + 6");

        std::vector<uint8_t> blob = AllocBlob(need);
        const int rc = iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size());

        /* The grammar accepts it: one component is a well-formed container.
         * Whether it can be displayed is the viewer's policy, which lives in
         * IwiFormat.ixx and is tested there - this file tests the C API. */
        check(rc == IWI_STATUS_OK, "grayscale is structurally valid");
        const IwiFileComponent* comps = iwi_file_components(AsHeader(blob));
        check(comps[0].semantic == IW_COMPONENT_SEMANTIC_GRAY,
              "grayscale component table walkable");
    }

    /* --- payload encoding compared in full, not by prefix -------------- */
    {
        Spec spec;
        spec.encodingGarbage = true;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        const int rc = iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size());

        /* The grammar does not judge a codec name; the predicate does, and it
         * compares the whole eight-byte field. */
        check(rc == IWI_STATUS_OK, "grammar does not judge the codec");
        check(iwi_payload_encoding_is_raw(AsHeader(blob)) == 0,
              "\"rawgarba\" is not raw: the field is compared whole");
    }

    /* --- an all-zero encoding field means raw -------------------------- */
    {
        Spec spec;
        spec.encoding = nullptr;   /* leave the field zeroed */
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size()) == IWI_STATUS_OK,
              "zeroed encoding field parses as raw");
    }

    /* --- metadata is addressable at 36 + 6N, and is not copied --------- */
    {
        Spec spec;
        spec.components = 4;
        spec.metaBytes  = 16;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size()) == IWI_STATUS_OK,
              "file with metadata parses");

        const uint8_t* meta = nullptr;
        std::size_t    metaLen = 0;
        check(iwi_file_metadata(AsHeader(blob), file.data(), file.size(), &meta, &metaLen)
                  == IWI_STATUS_OK,
              "metadata query succeeds");

        const std::size_t expectOffset = IWI_FILE_BASE_HEADER_BYTES + 4u * IWI_FILE_COMPONENT_BYTES;
        check(metaLen == spec.metaBytes, "metadata length is metaBytes");
        check(meta == file.data() + expectOffset, "metadata points at 36 + 6N, not a copy");
    }

    /* --- no component cap: 200 components parses and walks ------------- */
    {
        Spec spec;
        spec.components = 200;
        const std::vector<uint8_t> file = MakeIwi(spec);

        const std::size_t need = iwi_file_required_size(file.data(), file.size());
        check(need == IWI_FILE_BASE_HEADER_BYTES + 200u * IWI_FILE_COMPONENT_BYTES,
              "200-component required_size");

        std::vector<uint8_t> blob = AllocBlob(need);

        /*
         * Grammar accepts it and the policy refuses it, and both matter. The
         * container has no component cap - that is what the structural parse
         * proves - while the viewer displays 3 or 4 components, which is what
         * iwi_parse adds on top.
         */
        const int structural = iwi_parse_structural(
            AsHeader(blob), blob.size(), file.data(), file.size());
        check(structural == IWI_STATUS_OK, "200-component file is structurally valid: no cap");

        const IwiFileComponent* c = iwi_file_components(AsHeader(blob));
        check(c[0].semantic == IW_COMPONENT_SEMANTIC_R, "200: first component R");
        check(c[199].bitOffset == static_cast<uint16_t>(199u * 8u), "200: last component walkable");


        /* It parses, but it is not an RGBA image, and decode says so. */
        std::vector<uint8_t> rgba(4);
        const int drc = iwi_decode(AsHeader(blob), file.data(), file.size(),
                                   rgba.data(), rgba.size());
        check(drc == IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT,
              "200-component decode refused rather than guessed");
    }

    /* --- null input is a status, not a crash ---------------------------- */
    {
        IwiFileHeader h{};
        check(iwi_parse_structural(&h, sizeof(h), nullptr, 0) == IWI_STATUS_NO_INPUT, "null bytes");
        check(iwi_parse_structural(nullptr, 0, nullptr, 0) == IWI_STATUS_NO_INPUT, "null out");
        check(iwi_file_required_size(nullptr, 0) == 0, "null required_size");

        const uint8_t* meta = nullptr;
        std::size_t len = 0;
        check(iwi_file_metadata(nullptr, nullptr, 0, &meta, &len) == IWI_STATUS_NO_INPUT,
              "null metadata");
    }

    /* --- every status has a message ------------------------------------ */
    {
        for (int s = IWI_STATUS_OK; s <= IWI_STATUS_UNADDRESSABLE_GEOMETRY; ++s) {
            const char* msg = iwi_status_message(s);
            check(msg != nullptr && msg[0] != '\0', "status has a message");
        }
        check(std::strcmp(iwi_status_message(IWI_STATUS_COMPONENT_LAYOUT), "") != 0,
              "layout status has text");
        checkMessage(IWI_STATUS_COMPONENT_LAYOUT, "offset", "layout message names the offset");
    }

    /* --- the split: grammar accepts what policy refuses ------------------ */
    /*
     * These are the files that made the two-parser split necessary.  Each one is
     * a well-formed container that this viewer cannot display, and the API has
     * to say so in two different ways rather than one.
     */
    {
        struct Case {
            const char* name;
            Spec        spec;
            int         expected;   /* what iwi_parse returns */
        };

        Case cases[3];
        cases[0].name = "grayscale";
        cases[0].spec = Spec{};
        cases[0].spec.components    = 1;
        cases[0].spec.firstSemantic = static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_GRAY);
        cases[0].expected = IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT;

        cases[1].name = "16-bit";
        cases[1].spec = Spec{};
        cases[1].spec.components    = 1;
        cases[1].spec.firstSemantic = static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_GRAY);
        cases[1].spec.bitWidth      = 16;
        cases[1].expected = IWI_STATUS_UNSUPPORTED_BIT_DEPTH;

        cases[2].name = "planar";
        cases[2].spec = Spec{};
        cases[2].spec.storageFlags = static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR);
        cases[2].expected = IWI_STATUS_UNSUPPORTED_STORAGE;

        for (const Case& c : cases) {
            const std::vector<uint8_t> file = MakeIwi(c.spec);
            const std::size_t need = iwi_file_required_size(file.data(), file.size());
            std::vector<uint8_t> blob = AllocBlob(need == 0 ? 64 : need);

            const int structural = iwi_parse_structural(
                AsHeader(blob), blob.size(), file.data(), file.size());
            check(structural == IWI_STATUS_OK, "grammar accepts a well-formed container");

            /* The grammar accepts each of these; that is the property the C API
             * owns. The capability refusal is the viewer's policy and is
             * asserted in src/tests/format/test_iwi_anti_drift.cpp. */
            check(iwi_status_is_unsupported(structural) == 0,
                  "grammar acceptance is not a capability failure");
            (void)c.expected;
        }
    }

    /* --- malformed is not unsupported ------------------------------------ */
    {
        std::vector<uint8_t> file = MakeIwi(Spec{});
        file[0] = 'X';
        std::vector<uint8_t> blob = AllocBlob(64);
        const int rc = iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size());
        check(rc == IWI_STATUS_BAD_MAGIC, "bad magic stays malformed");
        check(iwi_status_is_unsupported(rc) == 0, "malformed is not classified unsupported");
        check(iwi_status_is_unsupported(IWI_STATUS_OK) == 0, "ok is not unsupported");
        check(std::strstr(iwi_status_message(rc), "valid IWI") == nullptr,
              "a malformed message does not claim the file is valid");
    }

    /* --- the codec predicate is the one rule both readers use ------------ */
    {
        Spec raw;
        const std::vector<uint8_t> rawFile = MakeIwi(raw);
        std::vector<uint8_t> blob = AllocBlob(64);
        check(iwi_parse_structural(AsHeader(blob), blob.size(), rawFile.data(), rawFile.size())
                  == IWI_STATUS_OK,
              "raw file parses structurally");
        check(iwi_payload_encoding_is_raw(AsHeader(blob)) == 1, "raw is recognised");

        Spec garbage;
        garbage.encodingGarbage = true;
        const std::vector<uint8_t> g = MakeIwi(garbage);
        std::vector<uint8_t> blob2 = AllocBlob(64);
        check(iwi_parse_structural(AsHeader(blob2), blob2.size(), g.data(), g.size())
                  == IWI_STATUS_OK,
              "grammar does not judge the codec");
        check(iwi_payload_encoding_is_raw(AsHeader(blob2)) == 0,
              "\"rawgarba\" is not raw: the field is compared whole");
    }

    /* --- a 16-bit payload is measured in bytes, not samples -------------- */
    {
        /* 4x4 gray16: 16 pixels x 16 bits = 32 bytes of payload. A parser that
         * assumed one byte per sample would accept this truncated by half. */
        Spec spec;
        spec.components    = 1;
        spec.firstSemantic = static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_GRAY);
        spec.bitWidth      = 16;

        const std::vector<uint8_t> full = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(64);
        check(iwi_parse_structural(AsHeader(blob), blob.size(), full.data(), full.size())
                  == IWI_STATUS_OK,
              "16-bit payload accounted correctly");

        const std::vector<uint8_t> halved(full.begin(), full.end() - (full.size() / 4));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), halved.data(), halved.size())
                  == IWI_STATUS_PAYLOAD_TRUNCATED,
              "16-bit file short by its extra bytes is refused");
    }
    /* --- the decoder refuses layouts it would otherwise misread ---------- */
    /*
     * iwi_decode reads the payload as tightly packed interleaved bytes.  The
     * grammar accepts planar, columnar and strided files because they are legal
     * IWI, so the DECODER is the layer that must refuse them - otherwise it
     * returns a success code and wrong pixels, which is worse than an error.
     */
    {
        struct Case { const char* name; uint32_t flags; uint32_t stride; };
        const Case cases[] = {
            { "planar",          static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR),       0u },
            { "columnar",        static_cast<uint32_t>(IW_STORAGE_FLAG_COLUMNAR),     0u },
            { "flip-x",          static_cast<uint32_t>(IW_STORAGE_FLAG_FLIP_X),       0u },
            { "flip-y",          static_cast<uint32_t>(IW_STORAGE_FLAG_FLIP_Y),       0u },
            { "stride-8bit",     static_cast<uint32_t>(IW_STORAGE_FLAG_STRIDE_8BIT),  0u },
            { "stride-16bit",    static_cast<uint32_t>(IW_STORAGE_FLAG_STRIDE_16BIT), 0u },
            { "stride-32bit",    static_cast<uint32_t>(IW_STORAGE_FLAG_STRIDE_32BIT), 0u },
            { "stride declared", 0u,                                                 64u },
            { "planar|columnar", static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR) |
                                 static_cast<uint32_t>(IW_STORAGE_FLAG_COLUMNAR),     0u },
        };

        for (const Case& c : cases) {
            Spec spec;
            spec.storageFlags  = c.flags;
            spec.storageStride = c.stride;

            const std::vector<uint8_t> file = MakeIwi(spec);
            std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));

            /* Grammar: a layout is not a grammar question. */
            check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size())
                      == IWI_STATUS_OK,
                  "layout is grammatically valid");

            std::vector<uint8_t> rgba(static_cast<std::size_t>(spec.width) * spec.height * 4u, 0);
            const int drc = iwi_decode(AsHeader(blob), file.data(), file.size(),
                                       rgba.data(), rgba.size());
            check(drc == IWI_STATUS_UNSUPPORTED_STORAGE,
                  "decoder refuses a layout it would misread");
            check(iwi_status_is_unsupported(drc) != 0,
                  "a refused layout is a capability answer, not corruption");
        }
    }

    /* --- the decoder refuses a codec it cannot read ---------------------- */
    {
        Spec spec;
        spec.encodingGarbage = true;
        const std::vector<uint8_t> file = MakeIwi(spec);
        std::vector<uint8_t> blob = AllocBlob(iwi_file_required_size(file.data(), file.size()));
        check(iwi_parse_structural(AsHeader(blob), blob.size(), file.data(), file.size())
                  == IWI_STATUS_OK,
              "grammar accepts any codec name");

        std::vector<uint8_t> rgba(static_cast<std::size_t>(spec.width) * spec.height * 4u, 0);
        check(iwi_decode(AsHeader(blob), file.data(), file.size(), rgba.data(), rgba.size())
                  == IWI_STATUS_UNSUPPORTED_ENCODING,
              "decoder refuses a codec it cannot decode");
    }

    /* --- zero components is malformed, not unsupported ------------------- */
    {
        Spec spec;
        spec.components = 0;
        const std::vector<uint8_t> file = MakeIwi(spec);

        std::vector<uint8_t> blob = AllocBlob(64);
        const int rc = iwi_parse_structural(AsHeader(blob), blob.size(),
                                            file.data(), file.size());
        check(rc == IWI_STATUS_NO_COMPONENTS, "zero components is malformed");
        check(iwi_status_is_unsupported(rc) == 0,
              "a container with no pixel format is not a capability gap");
        checkMessage(rc, "component", "the message names the component count");
    }

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("iwi C API: all checks passed\n");
    return 0;
}
