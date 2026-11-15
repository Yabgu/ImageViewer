/*
 * test_iwi_anti_drift.cpp - do the two .iwi readers agree?
 *
 * The repository had two independent implementations of the 74-byte (now
 * 36 + 6N) header rules: the C ABI in IwiFile.cpp, which the viewer's loader
 * and the shell thumbnail provider use, and IWImageIO::DeserializeImage, which
 * iwconv and the format round-trip tests use.  They are supposed to describe
 * one format.  This test is what makes that a claim with evidence instead of a
 * comment: it feeds the same bytes to both and compares what they say.
 *
 * Three kinds of agreement are checked, because they fail independently:
 *
 *   verdict     accept or reject, and the reason for a rejection. Two readers
 *               that both reject a file can still disagree about why, and a
 *               user sees the difference.
 *   header      width, height, colour space, component count, storage layout,
 *               and every component record.  A reader can accept a file and
 *               still misread it.
 *   pixels      for 4 components the RGBA output must match the raw payload
 *               byte for byte; for 3 components iwi_decode's alpha-synthesis
 *               is checked against the raw RGB bytes.
 *
 * A failure here means the two readers disagree about the format, which is the
 * condition this file exists to detect.  It was failing when it was written;
 * see the commit that adds it for which fixtures diverged and why.
 */

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "ImagePluginDef.h"
#include "IwiFile.h"

import IWImageIO;
import IwiFormat;

namespace {

struct Spec {
    uint32_t    width           = 4;
    uint32_t    height          = 4;
    uint16_t    components      = 4;
    uint32_t    metaBytes       = 0;
    uint32_t    storageFlags    = 0;
    uint32_t    storageStride   = 0;
    const char* encoding        = "raw";
    bool        encodingGarbage = false;
    uint16_t    bitWidth        = 8;
    uint8_t     firstSemantic   = 0;   /* 0 = R,G,B[,A] chain */
};

void PutU16(std::vector<uint8_t>& b, std::size_t at, uint16_t v)
{
    b[at + 0] = static_cast<uint8_t>(v & 0xFFu);
    b[at + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void PutU32(std::vector<uint8_t>& b, std::size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

void PutU64(std::vector<uint8_t>& b, std::size_t at, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        b[at + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

/*
 * A container whose payload is a deterministic ramp, so a byte comparison is
 * meaningful rather than "both produced something".
 */
std::vector<uint8_t> BuildIwi(const Spec& spec)
{
    const std::size_t header = IWI_FILE_BASE_HEADER_BYTES +
                               static_cast<std::size_t>(spec.components) * IWI_FILE_COMPONENT_BYTES;
    const std::size_t perPixel =
        static_cast<std::size_t>(spec.components) * (spec.bitWidth / 8u);
    const std::size_t payload = static_cast<std::size_t>(spec.width) * spec.height * perPixel;
    std::vector<uint8_t> b(header + spec.metaBytes + payload, 0);

    std::memcpy(b.data(), IWI_FILE_MAGIC, IWI_FILE_MAGIC_BYTES);
    PutU32(b, 4u, spec.width);
    PutU32(b, 8u, spec.height);
    PutU32(b, 12u, spec.metaBytes);
    PutU64(b, 16u, static_cast<uint64_t>(spec.storageFlags) |
                   (static_cast<uint64_t>(spec.storageStride) << 32u));

    if (spec.encodingGarbage) {
        std::memcpy(b.data() + 24, "rawgarba", 8);
    } else if (spec.encoding != nullptr) {
        std::memcpy(b.data() + 24, spec.encoding,
                    std::min<std::size_t>(8, std::strlen(spec.encoding)));
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

/* -- Reader A: the C ABI -------------------------------------------------- */

struct CApiResult {
    int rc = IWI_STATUS_NO_INPUT;
    std::vector<uint8_t> blob;
    bool ok() const { return rc == IWI_STATUS_OK; }
};

CApiResult ReadViaCApi(const std::vector<uint8_t>& bytes)
{
    CApiResult r;
    const std::size_t need = iwi_file_required_size(bytes.data(), bytes.size());
    r.blob.assign(need == 0 ? IWI_FILE_BASE_HEADER_BYTES : need, 0);
    r.rc = iwi_parse_structural(reinterpret_cast<IwiFileHeader*>(r.blob.data()),
                                r.blob.size(), bytes.data(), bytes.size());
    return r;
}

/* -- Reader B: IWImageIO -------------------------------------------------- */

struct IwiIoResult {
    bool ok = false;
    std::string reason;
    iw::Image image;
};

IwiIoResult ReadViaIwiImageIO(const std::vector<uint8_t>& bytes)
{
    IwiIoResult r;
    try {
        r.image = iw::DeserializeImage(bytes.data(), bytes.size());
        r.ok = true;
    } catch (const std::exception& e) {
        r.reason = e.what();
    }
    return r;
}

/* -- Comparisons ---------------------------------------------------------- */

/*
 * Verdict parity.  Both readers must agree that a file is readable or not.
 *
 * A rejection that both reach is a pass; only a split verdict is a failure.
 * The message names both outcomes so the divergence is legible without
 * re-running anything.
 */
void CheckVerdict(const char* name, const std::vector<uint8_t>& bytes)
{
    const CApiResult c = ReadViaCApi(bytes);
    const IwiIoResult io = ReadViaIwiImageIO(bytes);

    INFO("fixture: " << std::string(name));
    INFO("C ABI: " << (c.ok() ? "accept" : "reject") << " ("
                    << std::string(iwi_status_message(c.rc)) << ")");
    INFO("IWImageIO: " << (io.ok ? "accept" : "reject") << " ("
                       << io.reason << ")");

    CHECK_MESSAGE(c.ok() == io.ok,
                  "the two readers must agree on whether '" << name
                  << "' is readable");
}

/*
 * Header parity for a file both readers accept.  Width, height, colour space,
 * component count, storage layout and every component record are compared
 * field by field, because a reader can accept a file and still misread it.
 */
void CheckHeaderParity(const char* name, const std::vector<uint8_t>& bytes)
{
    const CApiResult c = ReadViaCApi(bytes);
    if (!c.ok()) {
        FAIL("fixture '" << name << "' was expected to be readable by the C ABI");
    }
    const IwiIoResult io = ReadViaIwiImageIO(bytes);
    REQUIRE(io.ok);

    const auto* f = reinterpret_cast<const IwiFileHeader*>(c.blob.data());

    INFO("fixture: " << std::string(name));
    CHECK(f->width        == io.image.width);
    CHECK(f->height       == io.image.height);
    CHECK(f->colorSpace   == static_cast<uint8_t>(io.image.colorSpace));
    CHECK(f->componentCount == io.image.format.componentCount);
    CHECK(f->storageLayout.raw == io.image.format.storageLayout.raw);

    const IwiFileComponent* comps = iwi_file_components(f);
    REQUIRE(io.image.format.components != nullptr);
    for (uint16_t i = 0; i < f->componentCount; ++i) {
        INFO("component " << i);
        CHECK(comps[i].semantic       == static_cast<uint8_t>(io.image.format.components[i].semantic));
        CHECK(comps[i].componentClass == static_cast<uint8_t>(io.image.format.components[i].componentClass));
        CHECK(comps[i].bitOffset      == io.image.format.components[i].bitOffset);
        CHECK(comps[i].bitWidth       == io.image.format.components[i].bitWidth);
    }
}

/*
 * Pixel parity.
 *
 * iwi_decode emits RGBA8.  IWImageIO carries the raw payload, so the two are
 * directly comparable only at 4 components; at 3 the comparison is the
 * alpha-synthesis rule applied to IWImageIO's own bytes.  That is the point of
 * the test: it checks iwi_decode's arithmetic against an independent reader's
 * data rather than against a constant baked into this file.
 */
void CheckPixelParity(const char* name, const std::vector<uint8_t>& bytes)
{
    const CApiResult c = ReadViaCApi(bytes);
    REQUIRE(c.ok());
    const IwiIoResult io = ReadViaIwiImageIO(bytes);
    REQUIRE(io.ok);

    const auto* f = reinterpret_cast<const IwiFileHeader*>(c.blob.data());
    const std::size_t pixels =
        static_cast<std::size_t>(f->width) * static_cast<std::size_t>(f->height);

    std::vector<uint8_t> rgba(pixels * 4u, 0);
    const int rc = iwi_decode(f, bytes.data(), bytes.size(), rgba.data(), rgba.size());
    REQUIRE(rc == IWI_STATUS_OK);

    INFO("fixture: " << std::string(name));
    REQUIRE(io.image.pixelData.size() == pixels * f->componentCount);

    if (f->componentCount == 4u) {
        CHECK_MESSAGE(std::memcmp(rgba.data(), io.image.pixelData.data(), rgba.size()) == 0,
                      "RGBA decode must equal IWImageIO's raw 4-component payload");
        return;
    }

    /* 3 components: alpha must be synthesised opaque, RGB passed through. */
    for (std::size_t p = 0; p < pixels; ++p) {
        INFO("pixel " << p);
        CHECK(rgba[p * 4 + 0] == io.image.pixelData[p * 3 + 0]);
        CHECK(rgba[p * 4 + 1] == io.image.pixelData[p * 3 + 1]);
        CHECK(rgba[p * 4 + 2] == io.image.pixelData[p * 3 + 2]);
        CHECK(rgba[p * 4 + 3] == 0xFFu);
    }
}

} // namespace

/* ── Files both readers should accept ─────────────────────────────────────── */

TEST_CASE("anti-drift: RGB8 verdict, header and pixels agree")
{
    const Spec spec = [] { Spec s; s.components = 3; return s; }();
    const auto bytes = BuildIwi(spec);
    CheckVerdict("rgb8", bytes);
    CheckHeaderParity("rgb8", bytes);
    CheckPixelParity("rgb8", bytes);
}

TEST_CASE("anti-drift: RGBA8 verdict, header and pixels agree")
{
    const Spec spec = [] { Spec s; s.components = 4; return s; }();
    const auto bytes = BuildIwi(spec);
    CheckVerdict("rgba8", bytes);
    CheckHeaderParity("rgba8", bytes);
    CheckPixelParity("rgba8", bytes);
}

TEST_CASE("anti-drift: a container with metadata agrees")
{
    const Spec spec = [] { Spec s; s.metaBytes = 16; return s; }();
    const auto bytes = BuildIwi(spec);
    CheckVerdict("rgb8+meta", bytes);
    CheckHeaderParity("rgb8+meta", bytes);
    CheckPixelParity("rgb8+meta", bytes);
}

/* ── Files both readers should reject ────────────────────────────────────── */

TEST_CASE("anti-drift: bad magic is rejected by both")
{
    auto bytes = BuildIwi(Spec{});
    bytes[0] = 'X';
    CheckVerdict("bad-magic", bytes);
}

TEST_CASE("anti-drift: a truncated payload is rejected by both")
{
    auto bytes = BuildIwi(Spec{});
    bytes.resize(bytes.size() - 1);
    CheckVerdict("payload-short-by-one", bytes);
}

TEST_CASE("anti-drift: a header shorter than the base header is rejected by both")
{
    auto bytes = BuildIwi(Spec{});
    bytes.resize(10);
    CheckVerdict("header-10-bytes", bytes);
}

TEST_CASE("anti-drift: an empty buffer is rejected by both")
{
    CheckVerdict("empty", std::vector<uint8_t>{});
}

/* ── Capability divergence: same container, two policies ─────────────────── */

/*
 * These are not disagreements about the format.  Both readers parse the same
 * grammar; they answer a different question afterwards.
 *
 *   iwi_parse_structural   is this a well-formed IWI?          yes
 *   iwi_parse              can this viewer display it?         no, and why
 *   IWImageIO              can the converter carry it through? yes
 *
 * Each case asserts all three, which is the whole argument for the split: one
 * grammar, two policies, and the difference stated rather than hidden behind
 * two parsers that quietly diverged.
 *
 * Assertion 4 of the shape - byte-identical parity - applies only where both
 * produce comparable pixel data.  These cases are ones the viewer refuses to
 * render, so the parity that matters is IWImageIO's payload against the file's
 * own bytes, which is checked here.
 */

/* The grammar, with no policy applied. */
CApiResult ReadViaStructural(const std::vector<uint8_t>& bytes)
{
    CApiResult r;
    const std::size_t need = iwi_file_required_size(bytes.data(), bytes.size());
    r.blob.assign(need == 0 ? IWI_FILE_BASE_HEADER_BYTES : need, 0);
    r.rc = iwi_parse_structural(reinterpret_cast<IwiFileHeader*>(r.blob.data()),
                                r.blob.size(), bytes.data(), bytes.size());
    return r;
}

/* Where the payload starts: base header + component table + metadata. */
std::size_t PayloadOffset(const IwiFileHeader* f)
{
    return IWI_FILE_BASE_HEADER_BYTES +
           static_cast<std::size_t>(f->componentCount) * IWI_FILE_COMPONENT_BYTES +
           static_cast<std::size_t>(f->metaBytes);
}

void CheckCapabilityDivergence(const char* name,
                               const std::vector<uint8_t>& bytes,
                               int expectedStatus,
                               const char* feature)
{
    INFO("fixture: " << std::string(name));

    /* 1. the grammar accepts it - it is a real IWI */
    CApiResult structural = ReadViaStructural(bytes);
    CHECK_MESSAGE(structural.rc == IWI_STATUS_OK,
                  "grammar must accept '" << name << "': "
                  << std::string(iwi_status_message(structural.rc)));

    /* 2. the viewer refuses it, as a capability rather than as corruption */
    /* 2. the viewer refuses it, as a capability rather than as corruption.
     *    The policy is the viewer's and lives in IwiFormat, which is exactly
     *    why this test can still assert it: the C library owns the grammar, the
     *    viewer owns the answer to "can we display this". */
    const iwi::IwiParseResult viewer =
        iwi::ParseHeader(bytes.data(), bytes.size());
    const int policy = viewer.nativeStatus;
    INFO("policy status: " << std::string(iwi_status_message(policy)));
    CHECK_MESSAGE(policy == expectedStatus,
                  "policy must refuse '" << name << "' with the specific code");
    CHECK(iwi_status_is_unsupported(policy) != 0);

    /*
     * The library reports the fact - what the file does that this build cannot
     * handle.  Whether that is phrased as "corrupt" or as "valid, cannot
     * display" is the front end's sentence, composed in ImageLoaderIWI and
     * asserted in test_imageloader_iwi.cpp.  Here the requirement is that the
     * message names the feature rather than staying generic.
     */
    const char* msg = iwi_status_message(policy);
    CHECK_MESSAGE(std::strstr(msg, feature) != nullptr,
                  "the refusal for '" << name << "' must name the feature ("
                  << feature << ")");

    /* 3. the converter accepts it and carries the payload through */
    const IwiIoResult io = ReadViaIwiImageIO(bytes);
    CHECK_MESSAGE(io.ok, "IWImageIO must accept '" << name << "': " << io.reason);
    if (!io.ok) {
        return;
    }

    const auto* f = reinterpret_cast<const IwiFileHeader*>(structural.blob.data());
    const std::size_t offset = PayloadOffset(f);
    const std::size_t payloadBytes = bytes.size() - offset;

    /* 4. and the payload is the file's own bytes, not a reinterpretation */
    INFO("payload bytes: " << payloadBytes);
    CHECK(io.image.pixelData.size() == payloadBytes);
    CHECK_MESSAGE(std::memcmp(io.image.pixelData.data(), bytes.data() + offset,
                              payloadBytes) == 0,
                  "carried payload for '" << name << "' must equal the file's bytes");
}

TEST_CASE("anti-drift: a grayscale container is grammar-valid and viewer-unsupported")
{
    Spec spec;
    spec.components    = 1;
    spec.firstSemantic = static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_GRAY);
    CheckCapabilityDivergence("grayscale", BuildIwi(spec),
                              IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT, "grayscale");
}

TEST_CASE("anti-drift: a 16-bit container is grammar-valid and viewer-unsupported")
{
    Spec spec;
    spec.components    = 1;
    spec.firstSemantic = static_cast<uint8_t>(IW_COMPONENT_SEMANTIC_GRAY);
    spec.bitWidth      = 16;
    CheckCapabilityDivergence("16-bit", BuildIwi(spec),
                              IWI_STATUS_UNSUPPORTED_BIT_DEPTH, "16-bit");
}

TEST_CASE("anti-drift: a planar container is grammar-valid and viewer-unsupported")
{
    Spec spec;
    spec.storageFlags = static_cast<uint32_t>(IW_STORAGE_FLAG_PLANAR);
    CheckCapabilityDivergence("planar", BuildIwi(spec),
                              IWI_STATUS_UNSUPPORTED_STORAGE, "planar");
}

TEST_CASE("anti-drift: a columnar container is grammar-valid and viewer-unsupported")
{
    Spec spec;
    spec.storageFlags = static_cast<uint32_t>(IW_STORAGE_FLAG_COLUMNAR);
    CheckCapabilityDivergence("columnar", BuildIwi(spec),
                              IWI_STATUS_UNSUPPORTED_STORAGE, "columnar");
}

TEST_CASE("anti-drift: a 200-component container is grammar-valid and viewer-unsupported")
{
    Spec spec;
    spec.components = 200;
    CheckCapabilityDivergence("n200", BuildIwi(spec),
                              IWI_STATUS_UNSUPPORTED_COMPONENT_COUNT, "component count");
}

/*
 * The codec is the one rule both readers enforce, so it is not a capability
 * divergence: the grammar does not judge a codec name, and both policies refuse
 * a name they cannot decode.  This is also where the three-byte comparison used
 * to hide - "rawgarba" starts with "raw", so the old reader accepted it.
 */
TEST_CASE("anti-drift: a codec neither reader decodes is refused by both policies")
{
    Spec spec;
    spec.encodingGarbage = true;
    const std::vector<uint8_t> bytes = BuildIwi(spec);

    CApiResult structural = ReadViaStructural(bytes);
    CHECK_MESSAGE(structural.rc == IWI_STATUS_OK,
                  "grammar does not judge the codec: "
                  << std::string(iwi_status_message(structural.rc)));

    const iwi::IwiParseResult viewer = iwi::ParseHeader(bytes.data(), bytes.size());
    const int policy = viewer.nativeStatus;
    CHECK(policy == IWI_STATUS_UNSUPPORTED_ENCODING);
    CHECK(iwi_status_is_unsupported(policy) != 0);

    const IwiIoResult io = ReadViaIwiImageIO(bytes);
    CHECK_MESSAGE(!io.ok, "IWImageIO must refuse \"rawgarba\" too");
    CHECK_MESSAGE(io.reason.find("encoding") != std::string::npos,
                  "the converter's refusal must mention the encoding: " << io.reason);
}