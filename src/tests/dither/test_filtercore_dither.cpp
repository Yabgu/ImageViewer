/*
 * test_filtercore_dither.cpp
 *
 * Unit tests for the FilterCore dithering algorithms.
 *
 * Covers:
 *   - ApplyNone   : truncation/rounding only (no dither)
 *   - ApplyOrdered: 4×4 Bayer ordered dither
 *   - ApplyFloydSteinberg: Floyd–Steinberg error diffusion
 *   - ApplyPWM    : temporal / PWM dither
 *
 * Key scenario: downsample a 16-bit-per-channel (UNORM16) image to 8bpc
 * (analogous to "30-bit / 10bpc PNG to 8bpc screen").  The tests verify that
 * intermediate values between two adjacent 8-bit levels are represented as a
 * mix of those two levels — i.e. that dithering actually occurs.
 *
 * Dependencies: ImagePluginDef.h, FilterPluginDef.h, FilterCore.hpp, Utils.hpp
 * No OpenGL or GLFW required — runs with IMAGEVIEWER_BUILD_VIEWER=OFF.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

#include "ImagePluginDef.h"
#include "FilterPluginDef.h"
#include "FilterCore.hpp"

// ─── Helpers ─────────────────────────────────────────────────────────────────

/* Build an interleaved RGB UNORM format with the given bits-per-channel. */
static IWImageFormat MakeRGBFormat(uint16_t bpc)
{
    IWImageFormat fmt = {};
    fmt.componentCount = 3;
    fmt.bitsPerPixel   = static_cast<uint16_t>(3 * bpc);
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UNORM,
                            static_cast<uint16_t>(0 * bpc), bpc };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UNORM,
                            static_cast<uint16_t>(1 * bpc), bpc };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UNORM,
                            static_cast<uint16_t>(2 * bpc), bpc };
    return fmt;
}

/*
 * Build an ImagePluginData backed by a flat pixel buffer.
 * The pixel buffer stores interleaved UNORM16 samples (2 bytes per channel).
 * value16 is the raw uint16 value written into every channel of every pixel.
 * The caller owns the buffer (pixels); pd.data points into it.
 */
static ImagePluginData MakeUniform16Image(int w, int h, uint16_t value16,
                                           std::vector<uint8_t>& pixels)
{
    const IWImageFormat fmt = MakeRGBFormat(16);
    const int bytesPerPixel = 6; /* 3 channels × 2 bytes each */
    const int stride        = w * bytesPerPixel;
    pixels.assign(static_cast<size_t>(h) * stride, 0);

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* pix = pixels.data() + static_cast<ptrdiff_t>(y) * stride
                           + x * bytesPerPixel;
            /* Write value16 little-endian into each of the 3 channels. */
            for (int c = 0; c < 3; ++c) {
                pix[c * 2 + 0] = static_cast<uint8_t>(value16 & 0xFF);
                pix[c * 2 + 1] = static_cast<uint8_t>(value16 >> 8);
            }
        }

    ImagePluginData pd = {};
    pd.width      = w;
    pd.height     = h;
    pd.stride     = stride;
    pd.colorSpace = IMAGE_COLOR_SPACE_LINEAR;
    pd.size       = pixels.size();
    pd.data       = pixels.data();
    pd.format     = fmt;
    return pd;
}

/* Extract the 8-bit R channel value from a packed RGB8 pixel at (x,y). */
static uint8_t ReadR8(const ImagePluginData* frame, int x, int y)
{
    const int bpp = 3;
    return frame->data[static_cast<ptrdiff_t>(y) * frame->stride + x * bpp];
}

// ─── IWScreenInfo helper ──────────────────────────────────────────────────────

/* 8bpc screen, 3 channels — matches the most common display configuration. */
static IWScreenInfo Screen8bpc()
{
    return IWScreenInfo{ 8, 3 };
}

// ─── Target format ────────────────────────────────────────────────────────────

static IWImageFormat Target8bpc()
{
    return FilterCore::MakeTargetFormat(Screen8bpc());
}

// ─── ApplyNone ────────────────────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyNone (no dithering)")
{
    TEST_CASE("exact 8bpc value passes through unchanged")
    {
        /* Source: UNORM16 value for 64/255 (exactly representable in 8bpc).
         * 64/255 = 0.25098… → UNORM16 = round(0.25098 × 65535) ≈ 16448 */
        const uint16_t src16 = static_cast<uint16_t>(64 * 65535u / 255u);
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(1, 1, src16, pixels);

        ImagePluginData* out = FilterCore::ApplyNone(pd, Target8bpc());
        REQUIRE(out != nullptr);

        const uint8_t r = ReadR8(out, 0, 0);
        /* Should round to the nearest 8-bit level.  64/255 × 65535 / 65535 × 255
         * = 64 exactly (the round-trip is loss-less).                           */
        CHECK(r == 64);

        FilterCore::FreeFrame(out);
    }

    TEST_CASE("mid-point value rounds to nearest 8-bit level")
    {
        /* UNORM16 value 32768 → normalised ≈ 0.50001
         * 0.50001 × 255 ≈ 127.50 → QuantizeRound → 128                         */
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(1, 1, 32768, pixels);

        ImagePluginData* out = FilterCore::ApplyNone(pd, Target8bpc());
        REQUIRE(out != nullptr);

        const uint8_t r = ReadR8(out, 0, 0);
        CHECK(r == 128);

        FilterCore::FreeFrame(out);
    }

    TEST_CASE("uniform image — all pixels get the same value")
    {
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(8, 4, 32768, pixels);

        ImagePluginData* out = FilterCore::ApplyNone(pd, Target8bpc());
        REQUIRE(out != nullptr);

        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 8; ++x)
                CHECK(ReadR8(out, x, y) == 128);

        FilterCore::FreeFrame(out);
    }
}

// ─── ApplyOrdered ─────────────────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyOrdered (Bayer dithering)")
{
    TEST_CASE("mid-point value produces mix of two adjacent 8-bit levels")
    {
        /*
         * UNORM16 = 32768 → normalised v ≈ 0.500007
         * v × 255 ≈ 127.502
         *
         * Bayer threshold for row=0:
         *   col 0: threshold = 0/16 → floor(127.502 + 0.000) = 127
         *   col 1: threshold = 8/16 → floor(127.502 + 0.500) = 128
         *   col 2: threshold = 2/16 → floor(127.502 + 0.125) = 127
         *   col 3: threshold = 10/16 → floor(127.502 + 0.625) = 128
         *
         * So the 4-pixel row should alternate 127, 128, 127, 128.
         */
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(4, 1, 32768, pixels);

        ImagePluginData* out = FilterCore::ApplyOrdered(pd, Target8bpc());
        REQUIRE(out != nullptr);

        CHECK(ReadR8(out, 0, 0) == 127);
        CHECK(ReadR8(out, 1, 0) == 128);
        CHECK(ReadR8(out, 2, 0) == 127);
        CHECK(ReadR8(out, 3, 0) == 128);

        FilterCore::FreeFrame(out);
    }

    TEST_CASE("exact 8bpc value is preserved (no spurious dither)")
    {
        /* v = 128/255 in UNORM16: raw = round(128/255 × 65535) = 32895 */
        const uint16_t src16 = static_cast<uint16_t>(128u * 65535u / 255u);
        std::vector<uint8_t> pixels;
        /* Use a 4×4 image to exercise all Bayer positions. */
        const ImagePluginData pd = MakeUniform16Image(4, 4, src16, pixels);

        ImagePluginData* out = FilterCore::ApplyOrdered(pd, Target8bpc());
        REQUIRE(out != nullptr);

        /* All pixels should map to 128 (the value is exactly representable). */
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                CHECK(ReadR8(out, x, y) == 128);

        FilterCore::FreeFrame(out);
    }

    TEST_CASE("16bpc-to-8bpc dither introduces both adjacent levels across 4x4 tile")
    {
        /*
         * Value 0.75 in UNORM16: 0.75 × 65535 ≈ 49151
         * 0.75 × 255 = 191.25
         * Most Bayer positions (threshold < 0.25) → 191, rest → 192.
         * The tile should contain both 191 and 192.
         */
        const uint16_t src16 = static_cast<uint16_t>(0.75f * 65535.0f);
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(4, 4, src16, pixels);

        ImagePluginData* out = FilterCore::ApplyOrdered(pd, Target8bpc());
        REQUIRE(out != nullptr);

        bool saw191 = false, saw192 = false;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const uint8_t v = ReadR8(out, x, y);
                if (v == 191) saw191 = true;
                if (v == 192) saw192 = true;
            }

        CHECK(saw191);
        CHECK(saw192);

        FilterCore::FreeFrame(out);
    }
}

// ─── ApplyFloydSteinberg ──────────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyFloydSteinberg (error diffusion)")
{
    TEST_CASE("single row: average output equals expected continuous value")
    {
        /*
         * A uniform 8×1 row at v ≈ 0.5 should produce some 127s and some 128s
         * whose average is close to 127.5.  The exact distribution is
         * deterministic because FS error diffusion is a fixed algorithm.
         *
         * We just verify: both 127 and 128 appear and that their integer average
         * is within ±1 of 128.
         */
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(8, 1, 32768, pixels);

        ImagePluginData* out = FilterCore::ApplyFloydSteinberg(pd, Target8bpc());
        REQUIRE(out != nullptr);

        bool saw127 = false, saw128 = false;
        int  sum    = 0;
        for (int x = 0; x < 8; ++x) {
            const int v = ReadR8(out, x, 0);
            if (v == 127) saw127 = true;
            if (v == 128) saw128 = true;
            sum += v;
        }
        /* Both adjacent levels must appear — that is the definition of dithering. */
        CHECK(saw127);
        CHECK(saw128);
        /* Average should be close to 127.5 (round to 128). */
        const int avg = (sum + 4) / 8; /* integer division with rounding */
        CHECK(avg == 128);

        FilterCore::FreeFrame(out);
    }

    TEST_CASE("exact 8bpc value produces zero error and constant output")
    {
        /* v = 64/255 exactly. FS error = 0, so every pixel should be 64. */
        const uint16_t src16 = static_cast<uint16_t>(64u * 65535u / 255u);
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(8, 2, src16, pixels);

        ImagePluginData* out = FilterCore::ApplyFloydSteinberg(pd, Target8bpc());
        REQUIRE(out != nullptr);

        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 8; ++x)
                CHECK(ReadR8(out, x, y) == 64);

        FilterCore::FreeFrame(out);
    }

    TEST_CASE("FS dither does not produce values outside adjacent levels")
    {
        /* For v ≈ 0.5, the output must only be 127 or 128 — never anything else. */
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(16, 4, 32768, pixels);

        ImagePluginData* out = FilterCore::ApplyFloydSteinberg(pd, Target8bpc());
        REQUIRE(out != nullptr);

        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 16; ++x) {
                const uint8_t v = ReadR8(out, x, y);
                CHECK((v == 127 || v == 128));
            }

        FilterCore::FreeFrame(out);
    }
}

// ─── ApplyPWM ─────────────────────────────────────────────────────────────────

TEST_SUITE("FilterCore::ApplyPWM (temporal dither)")
{
    TEST_CASE("frame count matches 2^pwmBits")
    {
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(2, 2, 32768, pixels);

        for (uint32_t bits = 1; bits <= 4; ++bits) {
            IWFilterImageSet* set = FilterCore::ApplyPWM(pd, Target8bpc(), bits);
            REQUIRE(set != nullptr);
            CHECK(set->frameCount == (1u << bits));
            FilterCore::FreeImageSet(set);
        }
    }

    TEST_CASE("PWM: average over all frames equals expected value for mid-tone")
    {
        /*
         * v ≈ 0.5, 2-bit PWM (4 frames).
         * lo = floor(0.5×255) = 127, hi = 128, frac = 0.5
         * hiCount = round(0.5 × 4) = 2
         * frames 0,1 → 128; frames 2,3 → 127  (or similar split)
         * Average per pixel = (2×128 + 2×127) / 4 = 127.5 → rounds to 128
         */
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(1, 1, 32768, pixels);
        const uint32_t pwmBits = 2;
        const uint32_t frameCount = 1u << pwmBits;

        IWFilterImageSet* set = FilterCore::ApplyPWM(pd, Target8bpc(), pwmBits);
        REQUIRE(set != nullptr);
        REQUIRE(set->frameCount == frameCount);

        int sum = 0;
        bool sawLo = false, sawHi = false;
        for (uint32_t f = 0; f < frameCount; ++f) {
            REQUIRE(set->frames[f] != nullptr);
            const int v = ReadR8(set->frames[f], 0, 0);
            sum += v;
            if (v == 127) sawLo = true;
            if (v == 128) sawHi = true;
        }
        /* Both adjacent levels must appear across frames. */
        CHECK(sawLo);
        CHECK(sawHi);
        /* Average over frames should equal the expected continuous value. */
        const int avg = (sum + static_cast<int>(frameCount) / 2)
                        / static_cast<int>(frameCount);
        CHECK(avg == 128);

        FilterCore::FreeImageSet(set);
    }

    TEST_CASE("PWM: maximum-brightness pixel stays at 255 in every frame")
    {
        const uint16_t fullWhite = 65535;
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(1, 1, fullWhite, pixels);

        IWFilterImageSet* set = FilterCore::ApplyPWM(pd, Target8bpc(), 2);
        REQUIRE(set != nullptr);

        for (uint32_t f = 0; f < set->frameCount; ++f) {
            REQUIRE(set->frames[f] != nullptr);
            CHECK(ReadR8(set->frames[f], 0, 0) == 255);
        }

        FilterCore::FreeImageSet(set);
    }

    TEST_CASE("PWM: black pixel stays at 0 in every frame")
    {
        std::vector<uint8_t> pixels;
        const ImagePluginData pd = MakeUniform16Image(1, 1, 0, pixels);

        IWFilterImageSet* set = FilterCore::ApplyPWM(pd, Target8bpc(), 2);
        REQUIRE(set != nullptr);

        for (uint32_t f = 0; f < set->frameCount; ++f) {
            REQUIRE(set->frames[f] != nullptr);
            CHECK(ReadR8(set->frames[f], 0, 0) == 0);
        }

        FilterCore::FreeImageSet(set);
    }
}

// ─── FreeImageSet ─────────────────────────────────────────────────────────────

TEST_SUITE("FilterCore memory management")
{
    TEST_CASE("FreeFrame(nullptr) is safe")
    {
        FilterCore::FreeFrame(nullptr);
    }

    TEST_CASE("FreeImageSet(nullptr) is safe")
    {
        FilterCore::FreeImageSet(nullptr);
    }
}
