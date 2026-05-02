#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include "ImagePluginDef.h"
#include "Utils.hpp"
#include "FilterPluginDef.h"
#include "FilterCore.hpp"

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

// ─── FilterCore helpers ───────────────────────────────────────────────────────

/* Build a 1×1 16-bit RGBA UNORM ImagePluginData with a given raw 16-bit value
 * in all colour channels and full alpha. */
static ImagePluginData* MakeRGBA16Pixel(uint16_t rgbVal, uint16_t aVal = 0xFFFFu)
{
    IWImageFormat fmt = {};
    fmt.componentCount = 4;
    fmt.bitsPerPixel   = 64;
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 16 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM, 16, 16 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 32, 16 };
    fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 48, 16 };

    uint8_t* buf = static_cast<uint8_t*>(std::malloc(8));
    uint16_t* p  = reinterpret_cast<uint16_t*>(buf);
    p[0] = rgbVal; p[1] = rgbVal; p[2] = rgbVal; p[3] = aVal;

    ImagePluginData* pd = static_cast<ImagePluginData*>(
        std::malloc(sizeof(ImagePluginData)));
    pd->width = 1; pd->height = 1; pd->stride = 8;
    pd->colorSpace = IMAGE_COLOR_SPACE_LINEAR;
    pd->size = 8; pd->data = buf; pd->format = fmt;
    return pd;
}

/* Build an w×1 16-bit RGBA UNORM row with every pixel set to the same value. */
static ImagePluginData* MakeRGBA16Row(int w, uint16_t rgbVal)
{
    IWImageFormat fmt = {};
    fmt.componentCount = 4;
    fmt.bitsPerPixel   = 64;
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,  0, 16 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM, 16, 16 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM, 32, 16 };
    fmt.components[3]  = { IW_COMPONENT_SEMANTIC_A, IW_COMPONENT_CLASS_UNORM, 48, 16 };

    const int stride = w * 8; /* 8 bytes per pixel */
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(stride)));
    for (int x = 0; x < w; ++x) {
        uint16_t* p = reinterpret_cast<uint16_t*>(buf + x * 8);
        p[0] = rgbVal; p[1] = rgbVal; p[2] = rgbVal; p[3] = 0xFFFFu;
    }

    ImagePluginData* pd = static_cast<ImagePluginData*>(
        std::malloc(sizeof(ImagePluginData)));
    pd->width = w; pd->height = 1; pd->stride = stride;
    pd->colorSpace = IMAGE_COLOR_SPACE_LINEAR;
    pd->size = static_cast<size_t>(stride); pd->data = buf; pd->format = fmt;
    return pd;
}

// ─── FilterCore::MakeTargetFormat ────────────────────────────────────────────

TEST_SUITE("FilterCore::MakeTargetFormat")
{
    TEST_CASE("8-bit screen → RGBA8 UNORM interleaved")
    {
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat fmt = FilterCore::MakeTargetFormat(screen);
        CHECK(fmt.componentCount == 4);
        CHECK(fmt.bitsPerPixel   == 32);
        CHECK(fmt.storageLayout  == IW_STORAGE_INTERLEAVED);
        CHECK(fmt.components[0].bitWidth  == 8);
        CHECK(fmt.components[0].semantic  == IW_COMPONENT_SEMANTIC_R);
        CHECK(fmt.components[0].componentClass == IW_COMPONENT_CLASS_UNORM);
        CHECK(fmt.components[3].semantic  == IW_COMPONENT_SEMANTIC_A);
        CHECK(fmt.components[3].bitOffset == 24);
    }

    TEST_CASE("10-bit screen → RGBA10 UNORM interleaved")
    {
        IWScreenInfo screen{ 10, 4 };
        const IWImageFormat fmt = FilterCore::MakeTargetFormat(screen);
        CHECK(fmt.componentCount  == 4);
        CHECK(fmt.bitsPerPixel    == 40);
        CHECK(fmt.components[0].bitWidth == 10);
    }

    TEST_CASE("zero bitsPerChannel defaults to 8")
    {
        IWScreenInfo screen{ 0, 4 };
        const IWImageFormat fmt = FilterCore::MakeTargetFormat(screen);
        CHECK(fmt.components[0].bitWidth == 8);
    }
}

// ─── FilterCore::ApplyNone ────────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyNone")
{
    TEST_CASE("16-bit → 8-bit: mid-scale value rounds correctly")
    {
        /* Source: 16-bit value 0x8000 = 32768.
         * Normalised: 32768 / 65535 ≈ 0.50007.
         * 8-bit quantised (round): round(0.50007 * 255) = round(127.52) = 128. */
        ImagePluginData* src = MakeRGBA16Pixel(0x8000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* out = FilterCore::ApplyNone(*src, target);
        REQUIRE(out != nullptr);
        CHECK(out->format.components[0].bitWidth == 8);
        CHECK(out->data[0] == 128); /* R */
        CHECK(out->data[1] == 128); /* G */
        CHECK(out->data[2] == 128); /* B */
        CHECK(out->data[3] == 255); /* A = full (0xFFFF → 255) */

        FilterCore::FreeFrame(out);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("16-bit → 8-bit: zero value → 0")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0x0000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* out = FilterCore::ApplyNone(*src, target);
        REQUIRE(out != nullptr);
        CHECK(out->data[0] == 0);
        CHECK(out->data[3] == 255);

        FilterCore::FreeFrame(out);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("16-bit → 8-bit: max value → 255")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0xFFFFu);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* out = FilterCore::ApplyNone(*src, target);
        REQUIRE(out != nullptr);
        CHECK(out->data[0] == 255);

        FilterCore::FreeFrame(out);
        FilterCore::FreeFrame(src);
    }
}

// ─── FilterCore::ApplyPWM ─────────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyPWM")
{
    TEST_CASE("pwmBits=1 produces exactly 2 frames")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0x8000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        IWFilterImageSet* set = FilterCore::ApplyPWM(*src, target, 1u);
        REQUIRE(set != nullptr);
        CHECK(set->frameCount == 2u);
        CHECK(set->frames    != nullptr);
        CHECK(set->frames[0] != nullptr);
        CHECK(set->frames[1] != nullptr);

        FilterCore::FreeImageSet(set);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("pwmBits=1: frame averages match original fractional value within 0.5 LSB")
    {
        /* Source 16-bit R value: 0x80FF = 33023.
         * Normalised: 33023/65535 ≈ 0.50389.
         * 8-bit scaled: 0.50389 * 255 ≈ 128.49.
         * lo=128, hi=129, frac≈0.49, hiCount=round(0.49*2)=round(0.98)=1.
         * Frame 0: hi=129, Frame 1: lo=128.  Average = 128.5.
         * 8-bit rounded (no PWM) = round(128.49) = 128.
         * PWM average (128.5) is closer to the true value (128.49). */
        ImagePluginData* src = MakeRGBA16Pixel(0x80FFu);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        IWFilterImageSet* set = FilterCore::ApplyPWM(*src, target, 1u);
        REQUIRE(set != nullptr);
        REQUIRE(set->frameCount == 2u);

        const uint8_t v0 = set->frames[0]->data[0]; /* R channel, frame 0 */
        const uint8_t v1 = set->frames[1]->data[0]; /* R channel, frame 1 */

        /* Each frame should be either 128 or 129. */
        CHECK(v0 >= 128); CHECK(v0 <= 129);
        CHECK(v1 >= 128); CHECK(v1 <= 129);
        /* Average of the two frames should be exactly 128.5. */
        const float avg = (static_cast<float>(v0) + static_cast<float>(v1)) * 0.5f;
        CHECK(avg == doctest::Approx(128.5f));

        FilterCore::FreeImageSet(set);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("pwmBits=0 treated as 1 (at least 2 frames)")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0x8000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        IWFilterImageSet* set = FilterCore::ApplyPWM(*src, target, 0u);
        REQUIRE(set != nullptr);
        CHECK(set->frameCount == 2u);

        FilterCore::FreeImageSet(set);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("pwmBits=2 produces exactly 4 frames")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0xAAAAu);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        IWFilterImageSet* set = FilterCore::ApplyPWM(*src, target, 2u);
        REQUIRE(set != nullptr);
        CHECK(set->frameCount == 4u);

        FilterCore::FreeImageSet(set);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("alpha is not PWM-dithered — all frames carry the same rounded alpha")
    {
        /* Alpha value 0x8000 → normalised ≈ 0.5 → rounded to 8-bit = 128. */
        ImagePluginData* src = MakeRGBA16Pixel(0x8000u, 0x8000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        IWFilterImageSet* set = FilterCore::ApplyPWM(*src, target, 2u);
        REQUIRE(set != nullptr);
        const uint8_t a0 = set->frames[0]->data[3];
        for (uint32_t f = 1; f < set->frameCount; ++f)
            CHECK(set->frames[f]->data[3] == a0);

        FilterCore::FreeImageSet(set);
        FilterCore::FreeFrame(src);
    }
}

// ─── FilterCore::ApplyOrdered ────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyOrdered")
{
    TEST_CASE("ordered dither at Bayer(0,3)=10/16 lifts dark pixel from 0 to 1")
    {
        /* 16-bit source value = 103 (same for all 4 pixels in a 4×1 row).
         * Normalised: 103/65535 ≈ 0.001572.
         * 8-bit scaled: 0.001572 * 255 ≈ 0.4008.
         * Without dither: floor(0.4008) = 0.
         * Bayer[0][3] = 10/16 = 0.625 → floor(0.4008 + 0.625) = floor(1.0258) = 1.
         * Pixel (3, 0) should be 1; pixel (0, 0) with Bayer=0 stays at 0. */
        ImagePluginData* src = MakeRGBA16Row(4, 103u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* out = FilterCore::ApplyOrdered(*src, target);
        REQUIRE(out != nullptr);

        /* Each pixel is 4 bytes (RGBA8).  Pixel at x=3 starts at byte 12. */
        CHECK(out->data[0]  == 0u); /* pixel x=0: Bayer=0,     floor(0.4008+0)     = 0 */
        CHECK(out->data[12] == 1u); /* pixel x=3: Bayer=10/16, floor(0.4008+0.625) = 1 */

        FilterCore::FreeFrame(out);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("ordered dither at Bayer(0,0)=0 does not add noise to pixel (0,0)")
    {
        /* Source value 103: scaled ≈ 0.4008.
         * Bayer[0][0] = 0/16 = 0 → floor(0.4008 + 0) = 0.
         * Pixel (0, 0) stays at 0. */
        ImagePluginData* src = MakeRGBA16Row(1, 103u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* out = FilterCore::ApplyOrdered(*src, target);
        REQUIRE(out != nullptr);
        CHECK(out->data[0] == 0u); /* pixel 0, R channel */

        FilterCore::FreeFrame(out);
        FilterCore::FreeFrame(src);
    }
}

// ─── FilterCore::ApplyFloydSteinberg ─────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyFloydSteinberg")
{
    TEST_CASE("output dimensions and format match target")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0x8000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* out = FilterCore::ApplyFloydSteinberg(*src, target);
        REQUIRE(out != nullptr);
        CHECK(out->width  == src->width);
        CHECK(out->height == src->height);
        CHECK(out->format.componentCount == 4u);
        CHECK(out->format.components[0].bitWidth == 8u);

        FilterCore::FreeFrame(out);
        FilterCore::FreeFrame(src);
    }

    TEST_CASE("1×1 image: FS output is same as NONE (no neighbours to diffuse to)")
    {
        ImagePluginData* src = MakeRGBA16Pixel(0x8000u);
        IWScreenInfo screen{ 8, 4 };
        const IWImageFormat target = FilterCore::MakeTargetFormat(screen);

        ImagePluginData* outNone = FilterCore::ApplyNone(*src, target);
        ImagePluginData* outFS   = FilterCore::ApplyFloydSteinberg(*src, target);
        REQUIRE(outNone != nullptr);
        REQUIRE(outFS   != nullptr);
        CHECK(outNone->data[0] == outFS->data[0]);

        FilterCore::FreeFrame(outNone);
        FilterCore::FreeFrame(outFS);
        FilterCore::FreeFrame(src);
    }
}
