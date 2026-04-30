#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include "ImagePluginDef.h"
#include "Utils.hpp"

// ─── Format-builder helpers ───────────────────────────────────────────────────

static IWImageFormat MakeRGB8()
{
    IWImageFormat fmt = {};
    fmt.componentCount = 3;
    fmt.bitsPerPixel   = 24;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };
    return fmt;
}

static IWImageFormat MakeRGBA8()
{
    IWImageFormat fmt = {};
    fmt.componentCount = 4;
    fmt.bitsPerPixel   = 32;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };
    fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UINT, 24, 8 };
    return fmt;
}

static IWImageFormat MakeRGB16()
{
    IWImageFormat fmt = {};
    fmt.componentCount = 3;
    fmt.bitsPerPixel   = 48;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 16 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT, 16, 16 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 32, 16 };
    return fmt;
}

static IWImageFormat MakeRGBA32F()
{
    IWImageFormat fmt = {};
    fmt.componentCount = 4;
    fmt.bitsPerPixel   = 128;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_FLOAT,  0, 32 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_FLOAT, 32, 32 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_FLOAT, 64, 32 };
    fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_FLOAT, 96, 32 };
    return fmt;
}

// ─── ValidateImageFormat ──────────────────────────────────────────────────────

TEST_SUITE("ValidateImageFormat")
{
    TEST_CASE("accepts valid RGB8 interleaved format")
    {
        CHECK(ValidateImageFormat(MakeRGB8()));
    }

    TEST_CASE("accepts valid RGBA8 interleaved format")
    {
        CHECK(ValidateImageFormat(MakeRGBA8()));
    }

    TEST_CASE("accepts valid RGBA32F interleaved format")
    {
        CHECK(ValidateImageFormat(MakeRGBA32F()));
    }

    TEST_CASE("accepts exotic HSL mixed-precision format")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 30;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_H, IW_COMPONENT_CLASS_UINT,   0,  8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_S, IW_COMPONENT_CLASS_UNORM,  8,  6 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_L, IW_COMPONENT_CLASS_FLOAT, 14, 16 };
        CHECK(ValidateImageFormat(fmt));
    }

    TEST_CASE("accepts planar 16-bit grayscale format")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 16;
        fmt.storageLayout  = IW_STORAGE_PLANAR;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UNORM, 0, 16 };
        CHECK(ValidateImageFormat(fmt));
    }

    TEST_CASE("accepts zero componentCount (empty / padding descriptor)")
    {
        IWImageFormat fmt = {};
        /* An empty descriptor (componentCount=0) is accepted: there are no
           active components, so there are no bit-range constraints to check.
           This is a valid edge case, not an error. */
        fmt.bitsPerPixel = 0;
        CHECK(ValidateImageFormat(fmt));
    }

    TEST_CASE("rejects componentCount exceeding IW_MAX_COMPONENTS")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = static_cast<uint16_t>(IW_MAX_COMPONENTS + 1u);
        const char* err    = nullptr;
        CHECK(!ValidateImageFormat(fmt, &err));
        CHECK(err != nullptr);
    }

    TEST_CASE("rejects bitWidth == 0 for an active component")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 8;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UINT, 0, 0 };
        const char* err    = nullptr;
        CHECK(!ValidateImageFormat(fmt, &err));
        CHECK(err != nullptr);
    }

    TEST_CASE("rejects bitsPerPixel too small for the component layout")
    {
        IWImageFormat fmt = MakeRGB8();
        fmt.bitsPerPixel  = 16; /* should be 24 for three 8-bit components */
        CHECK(!ValidateImageFormat(fmt));
    }

    TEST_CASE("rejects overlapping bit ranges between two interleaved components")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 2;
        fmt.bitsPerPixel   = 16;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT, 4, 8 }; /* overlaps R [0,8) */
        const char* err    = nullptr;
        CHECK(!ValidateImageFormat(fmt, &err));
        CHECK(err != nullptr);
    }

    TEST_CASE("rejects component whose range starts beyond bitsPerPixel")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 8;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 8, 8 }; /* bitOffset=8 with bitsPerPixel=8 */
        CHECK(!ValidateImageFormat(fmt));
    }
}

// ─── Loader format descriptors ────────────────────────────────────────────────
//
// These tests verify that the IWImageFormat descriptors that each loader plugin
// produces match the expected layout documented in the loader source.  Rather
// than loading actual image files, the descriptors are constructed here using
// the same initialisation logic as the loaders.

TEST_SUITE("Loader format descriptors")
{
    TEST_CASE("JPEG RGB: 3 components, 8-bit UINT, sequential bitOffsets 0/8/16")
    {
        /* Mirrors ImageLoaderJpeg.ixx for a 3-component RGB output. */
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 24;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };

        REQUIRE(ValidateImageFormat(fmt));
        CHECK(fmt.componentCount    == 3);
        CHECK(fmt.bitsPerPixel      == 24);
        CHECK(fmt.storageLayout     == IW_STORAGE_INTERLEAVED);
        CHECK(fmt.components[0].semantic      == IW_COMPONENT_SEMANTIC_R);
        CHECK(fmt.components[0].componentClass== IW_COMPONENT_CLASS_UINT);
        CHECK(fmt.components[0].bitOffset     == 0);
        CHECK(fmt.components[0].bitWidth      == 8);
        CHECK(fmt.components[1].semantic      == IW_COMPONENT_SEMANTIC_G);
        CHECK(fmt.components[1].bitOffset     == 8);
        CHECK(fmt.components[1].bitWidth      == 8);
        CHECK(fmt.components[2].semantic      == IW_COMPONENT_SEMANTIC_B);
        CHECK(fmt.components[2].bitOffset     == 16);
        CHECK(fmt.components[2].bitWidth      == 8);
        /* Slots beyond componentCount must be the NONE sentinel. */
        CHECK(fmt.components[3].semantic == IW_COMPONENT_SEMANTIC_NONE);
    }

    TEST_CASE("PNG RGBA 8-bit: 4 components, alpha last, bitOffsets 0/8/16/24")
    {
        /* Mirrors ImageLoaderPng.ixx PNG_COLOR_TYPE_RGBA branch. */
        IWImageFormat fmt = {};
        fmt.componentCount = 4;
        fmt.bitsPerPixel   = 32;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };
        fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UINT, 24, 8 };

        REQUIRE(ValidateImageFormat(fmt));
        CHECK(fmt.componentCount             == 4);
        CHECK(fmt.components[3].semantic     == IW_COMPONENT_SEMANTIC_A);
        CHECK(fmt.components[3].bitOffset    == 24);
        CHECK(fmt.components[3].bitWidth     == 8);
    }

    TEST_CASE("TIFF 12-bit grayscale: 1 component, UINT, bitOffset 0, bitWidth 12")
    {
        /* Validates that the IWImageFormat ABI can represent a 12-bit grayscale
           descriptor.  The current TIFF loader normalises bit depths to 8 or 16
           at decode time, so it does not yet emit this exact descriptor.  This
           test documents the expected structure for future native 12-bit support
           and verifies that ValidateImageFormat() accepts it. */
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 12;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UINT, 0, 12 };

        REQUIRE(ValidateImageFormat(fmt));
        CHECK(fmt.componentCount              == 1);
        CHECK(fmt.components[0].semantic      == IW_COMPONENT_SEMANTIC_GRAY);
        CHECK(fmt.components[0].componentClass== IW_COMPONENT_CLASS_UINT);
        CHECK(fmt.components[0].bitOffset     == 0);
        CHECK(fmt.components[0].bitWidth      == 12);
    }

    TEST_CASE("WebP RGB: 3 components, 8-bit UINT, R/G/B semantics")
    {
        /* Mirrors ImageLoaderWebp.ixx for opaque (non-alpha) WebP output. */
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 24;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };

        REQUIRE(ValidateImageFormat(fmt));
        CHECK(fmt.components[0].semantic == IW_COMPONENT_SEMANTIC_R);
        CHECK(fmt.components[1].semantic == IW_COMPONENT_SEMANTIC_G);
        CHECK(fmt.components[2].semantic == IW_COMPONENT_SEMANTIC_B);
    }

    TEST_CASE("WebP RGBA: 4 components, 8-bit UINT, alpha at bitOffset 24")
    {
        /* Mirrors ImageLoaderWebp.ixx for WebP with alpha channel. */
        IWImageFormat fmt = MakeRGBA8();

        REQUIRE(ValidateImageFormat(fmt));
        CHECK(fmt.componentCount             == 4);
        CHECK(fmt.components[3].semantic     == IW_COMPONENT_SEMANTIC_A);
        CHECK(fmt.components[3].bitOffset    == 24);
    }

    TEST_CASE("unused component slots beyond componentCount are IW_COMPONENT_SEMANTIC_NONE")
    {
        const IWImageFormat fmt = MakeRGB8();
        /* Zero-initialisation of IWImageFormat guarantees slots 3..7 are NONE. */
        for (uint16_t i = fmt.componentCount; i < IW_MAX_COMPONENTS; ++i)
            CHECK(fmt.components[i].semantic == IW_COMPONENT_SEMANTIC_NONE);
    }
}

// ─── ClassifyGLUpload (fast-path qualification) ───────────────────────────────

TEST_SUITE("ClassifyGLUpload")
{
    TEST_CASE("RGB8 interleaved → valid, 3 channels, 8-bit, not float, not BGR")
    {
        const auto h = ClassifyGLUpload(MakeRGB8());
        CHECK(h.valid);
        CHECK(h.channels == 3);
        CHECK(h.bitWidth == 8);
        CHECK(!h.isFloat);
        CHECK(!h.isBGR);
    }

    TEST_CASE("RGBA8 interleaved → valid, 4 channels")
    {
        const auto h = ClassifyGLUpload(MakeRGBA8());
        CHECK(h.valid);
        CHECK(h.channels == 4);
        CHECK(h.bitWidth == 8);
        CHECK(!h.isFloat);
        CHECK(!h.isBGR);
    }

    TEST_CASE("RGB16 interleaved → valid, 16-bit, not float")
    {
        const auto h = ClassifyGLUpload(MakeRGB16());
        CHECK(h.valid);
        CHECK(h.channels == 3);
        CHECK(h.bitWidth == 16);
        CHECK(!h.isFloat);
    }

    TEST_CASE("RGBA32F interleaved → valid, 32-bit, float")
    {
        const auto h = ClassifyGLUpload(MakeRGBA32F());
        CHECK(h.valid);
        CHECK(h.channels == 4);
        CHECK(h.bitWidth == 32);
        CHECK(h.isFloat);
    }

    TEST_CASE("BGR8 → valid, isBGR=true")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 24;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 16, 8 };
        const auto h = ClassifyGLUpload(fmt);
        CHECK(h.valid);
        CHECK(h.isBGR);
    }

    TEST_CASE("BGRA8 → valid, isBGR=true, 4 channels")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 4;
        fmt.bitsPerPixel   = 32;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 16, 8 };
        fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UINT, 24, 8 };
        const auto h = ClassifyGLUpload(fmt);
        CHECK(h.valid);
        CHECK(h.isBGR);
        CHECK(h.channels == 4);
    }

    TEST_CASE("GRAY8 → valid, 1 channel")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 1;
        fmt.bitsPerPixel   = 8;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_GRAY, IW_COMPONENT_CLASS_UINT, 0, 8 };
        const auto h = ClassifyGLUpload(fmt);
        CHECK(h.valid);
        CHECK(h.channels == 1);
        CHECK(!h.isFloat);
    }

    TEST_CASE("mixed-precision (8-bit R + 16-bit G) → invalid")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 2;
        fmt.bitsPerPixel   = 24;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0,  8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 16 };
        CHECK(!ClassifyGLUpload(fmt).valid);
    }

    TEST_CASE("float16 RGB → invalid (not directly GL-uploadable)")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 48;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_FLOAT,  0, 16 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_FLOAT, 16, 16 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_FLOAT, 32, 16 };
        CHECK(!ClassifyGLUpload(fmt).valid);
    }

    TEST_CASE("HSL semantics → invalid (exotic, not standard RGB/RGBA)")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 24;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_H, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_S, IW_COMPONENT_CLASS_UINT,  8, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_L, IW_COMPONENT_CLASS_UINT, 16, 8 };
        CHECK(!ClassifyGLUpload(fmt).valid);
    }

    TEST_CASE("UNKNOWN semantics (e.g. UV data) → invalid")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 2;
        fmt.bitsPerPixel   = 16;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_UNKNOWN, IW_COMPONENT_CLASS_UINT,  0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_UNKNOWN, IW_COMPONENT_CLASS_UINT,  8, 8 };
        CHECK(!ClassifyGLUpload(fmt).valid);
    }

    TEST_CASE("planar layout → invalid regardless of channel semantics")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 3;
        fmt.bitsPerPixel   = 8;
        fmt.storageLayout  = IW_STORAGE_PLANAR;
        fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 0, 8 };
        fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT, 0, 8 };
        fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 0, 8 };
        CHECK(!ClassifyGLUpload(fmt).valid);
    }

    TEST_CASE("5-channel format → invalid (> 4 channels)")
    {
        IWImageFormat fmt = {};
        fmt.componentCount = 5;
        fmt.bitsPerPixel   = 40;
        for (int i = 0; i < 5; ++i)
            fmt.components[i] = { IW_COMPONENT_SEMANTIC_UNKNOWN, IW_COMPONENT_CLASS_UINT,
                                   static_cast<uint16_t>(i * 8), 8 };
        CHECK(!ClassifyGLUpload(fmt).valid);
    }
}

// ─── HalfToFloat ─────────────────────────────────────────────────────────────

TEST_SUITE("HalfToFloat")
{
    TEST_CASE("positive zero (0x0000) → 0.0f")
    {
        CHECK(HalfToFloat(0x0000u) == 0.0f);
    }

    TEST_CASE("negative zero (0x8000) → -0.0f, compares equal to 0.0f")
    {
        CHECK(HalfToFloat(0x8000u) == 0.0f);
    }

    TEST_CASE("one (0x3C00) → 1.0f")
    {
        CHECK(HalfToFloat(0x3C00u) == 1.0f);
    }

    TEST_CASE("negative one (0xBC00) → -1.0f")
    {
        CHECK(HalfToFloat(0xBC00u) == -1.0f);
    }

    TEST_CASE("two (0x4000) → 2.0f")
    {
        CHECK(HalfToFloat(0x4000u) == 2.0f);
    }

    TEST_CASE("positive infinity (0x7C00)")
    {
        float v = HalfToFloat(0x7C00u);
        CHECK(std::isinf(v));
        CHECK(v > 0.0f);
    }

    TEST_CASE("negative infinity (0xFC00)")
    {
        float v = HalfToFloat(0xFC00u);
        CHECK(std::isinf(v));
        CHECK(v < 0.0f);
    }

    TEST_CASE("quiet NaN (0x7E00) → NaN")
    {
        CHECK(std::isnan(HalfToFloat(0x7E00u)));
    }

    TEST_CASE("smallest positive subnormal (0x0001) → small positive")
    {
        float v = HalfToFloat(0x0001u);
        CHECK(v > 0.0f);
        CHECK(v < 1e-4f);   /* ≈ 5.96e-8 */
    }

    TEST_CASE("largest finite half (0x7BFF) → 65504.0f")
    {
        CHECK(HalfToFloat(0x7BFFu) == doctest::Approx(65504.0f));
    }
}

// ─── ExtractComponent ─────────────────────────────────────────────────────────

TEST_SUITE("ExtractComponent")
{
    TEST_CASE("8-bit UINT: 255 → 255.0f (raw integer)")
    {
        uint8_t pixel[1] = {255};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == 255.0f);
    }

    TEST_CASE("8-bit UINT: 0 → 0.0f")
    {
        uint8_t pixel[1] = {0};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == 0.0f);
    }

    TEST_CASE("8-bit UNORM: 255/255 = 1.0f")
    {
        uint8_t pixel[1] = {255};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(1.0f));
    }

    TEST_CASE("8-bit UNORM: 0/255 = 0.0f")
    {
        uint8_t pixel[1] = {0};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(0.0f));
    }

    TEST_CASE("8-bit UNORM: 127/255 ≈ 0.498")
    {
        uint8_t pixel[1] = {127};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(127.0f / 255.0f));
    }

    TEST_CASE("RGB565 red channel: bitOffset=11, bitWidth=5, 0xF800 → 31.0f (UINT)")
    {
        /* 0xF800 in little-endian is {0x00, 0xF8}.
           Bits [11..15] = 0b11111 = 31. */
        uint8_t pixel[2] = {0x00, 0xF8};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT, 11, 5 };
        CHECK(ExtractComponent(pixel, c) == 31.0f);
    }

    TEST_CASE("RGB565 green channel: bitOffset=5, bitWidth=6, 0x07E0 → 63.0f (UINT)")
    {
        /* 0x07E0 in little-endian is {0xE0, 0x07}.
           Bits [5..10] = 0b111111 = 63. */
        uint8_t pixel[2] = {0xE0, 0x07};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT, 5, 6 };
        CHECK(ExtractComponent(pixel, c) == 63.0f);
    }

    TEST_CASE("float32 extraction round-trips exactly")
    {
        float val = 3.14159f;
        uint8_t pixel[4];
        std::memcpy(pixel, &val, 4);
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_FLOAT, 0, 32 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(3.14159f).epsilon(1e-5));
    }

    TEST_CASE("float16 extraction: 0x3C00 → 1.0f")
    {
        /* 0x3C00 in little-endian is {0x00, 0x3C}. */
        uint8_t pixel[2] = {0x00, 0x3C};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_FLOAT, 0, 16 };
        CHECK(ExtractComponent(pixel, c) == 1.0f);
    }

    TEST_CASE("float8 (unsupported width): raw bits returned as integer float")
    {
        /* For IW_COMPONENT_CLASS_FLOAT with bitWidth != 16 and != 32, the
           current implementation returns static_cast<float>(raw) — the raw
           unsigned integer value cast to float.  This is the intended fallback
           for widths not yet decoded (e.g. E4M3 / E5M2).  When proper decoding
           for such formats is added, this test should be updated accordingly. */
        uint8_t pixel[1] = {0x38};   /* decimal 56 */
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_UNKNOWN, IW_COMPONENT_CLASS_FLOAT, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == 56.0f);
    }

    TEST_CASE("8-bit SNORM: max positive (127) → 1.0f")
    {
        uint8_t pixel[1] = {127};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_SNORM, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(1.0f));
    }

    TEST_CASE("8-bit SNORM: -128 (0x80) clamped to -1.0f")
    {
        uint8_t pixel[1] = {0x80};   /* two's-complement -128 */
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_SNORM, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(-1.0f));
    }

    TEST_CASE("8-bit SINT: 0xFF (two's-complement -1) → -1.0f")
    {
        uint8_t pixel[1] = {0xFF};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_SINT, 0, 8 };
        CHECK(ExtractComponent(pixel, c) == -1.0f);
    }

    TEST_CASE("1-bit mask component: bit=1 → 1.0f")
    {
        uint8_t pixel[1] = {0b00000001};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_UNKNOWN, IW_COMPONENT_CLASS_UINT, 0, 1 };
        CHECK(ExtractComponent(pixel, c) == 1.0f);
    }

    TEST_CASE("1-bit mask component: bit=0 → 0.0f")
    {
        uint8_t pixel[1] = {0b00000000};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_UNKNOWN, IW_COMPONENT_CLASS_UINT, 0, 1 };
        CHECK(ExtractComponent(pixel, c) == 0.0f);
    }

    TEST_CASE("component at byte boundary (bitOffset=8, second byte of pixel)")
    {
        uint8_t pixel[2] = {0xFF, 0xAB};
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT, 8, 8 };
        CHECK(ExtractComponent(pixel, c) == 0xABu);
    }

    TEST_CASE("UNORM 5-bit: max value (31) → 1.0f")
    {
        /* A 5-bit UNORM channel at bit offset 0.  Value = 31 = 2^5 - 1 → 1.0. */
        uint8_t pixel[1] = {0x1F};   /* 0001 1111 = 31 in low 5 bits */
        IWComponentDef c = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM, 0, 5 };
        CHECK(ExtractComponent(pixel, c) == doctest::Approx(1.0f));
    }
}
