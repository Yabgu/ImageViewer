#include <doctest/doctest.h>

#include "Config.hpp"
#include "Args.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

namespace {

struct TempDir
{
    std::filesystem::path path;

    TempDir()
    {
        static int counter = 0;
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        REQUIRE_FALSE(ec);
        path = base / ("iview_config_test_" + std::to_string(++counter));
        std::filesystem::remove_all(path, ec);
        ec.clear();
        std::filesystem::create_directories(path, ec);
        REQUIRE_FALSE(ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << text;
    REQUIRE(out.good());
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

Config::Settings MakeNonDefault()
{
    Config::Settings s;
    s.canvasBg[0] = 10;
    s.canvasBg[1] = 20;
    s.canvasBg[2] = 30;
    s.fadeDelayMs = 4200;
    s.fadeMinAlpha = 0.35f;
    s.filterMode = Config::kFilterModeSpatialTemporal;
    s.pwmBits = 1;
    s.windowWidth = 1024;
    s.windowHeight = 768;
    s.windowX = 120;
    s.windowY = 60;
    s.windowMaximized = true;
    s.initialZoomFit = false;
    s.chromeVisible = false;
    s.wheelAction = 1;
    s.wheelNavThreshold = 2.5f;
    s.navZoomMode = 1;
    s.uiScale = 1.5f;
    s.showReadout = false;
    s.showControlStrip = false;
    s.rememberWindowSize = false;
    s.hdrMode = Config::HdrMode::Auto;
    s.hdrTransfer = Config::HdrTransfer::Pq;
    s.hdrSdrWhiteNits = 203;
    s.hdrPeakNits = 1000;
    s.hdrToneMap = false;
    return s;
}

void CheckValuesEqual(const Config::Settings& a, const Config::Settings& b)
{
    for (int i = 0; i < 3; ++i)
        CHECK(a.canvasBg[i] == b.canvasBg[i]);
    CHECK(a.fadeDelayMs == b.fadeDelayMs);
    CHECK(a.fadeMinAlpha == doctest::Approx(b.fadeMinAlpha));
    CHECK(a.filterMode == b.filterMode);
    CHECK(a.pwmBits == b.pwmBits);
    CHECK(a.windowWidth == b.windowWidth);
    CHECK(a.windowHeight == b.windowHeight);
    CHECK(a.windowX == b.windowX);
    CHECK(a.windowY == b.windowY);
    CHECK(a.windowMaximized == b.windowMaximized);
    CHECK(a.initialZoomFit == b.initialZoomFit);
    CHECK(a.chromeVisible == b.chromeVisible);
    CHECK(a.wheelAction == b.wheelAction);
    CHECK(a.wheelNavThreshold == doctest::Approx(b.wheelNavThreshold));
    CHECK(a.navZoomMode == b.navZoomMode);
    CHECK(a.uiScale == doctest::Approx(b.uiScale));
    CHECK(a.showReadout == b.showReadout);
    CHECK(a.showControlStrip == b.showControlStrip);
    CHECK(a.rememberWindowSize == b.rememberWindowSize);
    CHECK(a.hdrMode == b.hdrMode);
    CHECK(a.hdrTransfer == b.hdrTransfer);
    CHECK(a.hdrSdrWhiteNits == b.hdrSdrWhiteNits);
    CHECK(a.hdrPeakNits == b.hdrPeakNits);
    CHECK(a.hdrToneMap == b.hdrToneMap);
}

} // namespace

TEST_SUITE("Config")
{
    TEST_CASE("defaults match the documented values")
    {
        Config::Settings s;

        CHECK(s.canvasBg[0] == 128);
        CHECK(s.canvasBg[1] == 128);
        CHECK(s.canvasBg[2] == 128);
        CHECK(s.fadeDelayMs == 1500);
        CHECK(s.fadeMinAlpha == doctest::Approx(0.15f));
        CHECK(s.filterMode == Config::kFilterModeNone);
        CHECK(s.pwmBits == 1u);
        CHECK(s.windowWidth == 1280);
        CHECK(s.windowHeight == 720);
        CHECK(s.windowX == Config::kWindowPositionUnset);
        CHECK(s.windowY == Config::kWindowPositionUnset);
        CHECK_FALSE(s.windowMaximized);
        CHECK(s.initialZoomFit);
        CHECK(s.chromeVisible);
        CHECK(s.wheelAction == 0);
        CHECK(s.wheelNavThreshold == doctest::Approx(1.0f));
        CHECK(s.navZoomMode == 0);
        CHECK(s.uiScale == doctest::Approx(2.0f));
        CHECK(s.showReadout);
        CHECK(s.showControlStrip);
        CHECK(s.rememberWindowSize);
        CHECK(s.hdrMode == Config::HdrMode::Off);
        CHECK(s.hdrTransfer == Config::HdrTransfer::ScRgb);
        CHECK(s.hdrSdrWhiteNits == 0);
        CHECK(s.hdrPeakNits == 0);
        CHECK(s.hdrToneMap);
        CHECK(s.loadPath.empty());
        CHECK(s.savePath.empty());
    }

    TEST_CASE("SetDefaults resets the new fields too")
    {
        Config::Settings s;
        s.uiScale = 1.0f;
        s.hdrMode = Config::HdrMode::Force;
        s.hdrPeakNits = 4000;

        Config::SetDefaults(s);

        CHECK(s.uiScale == doctest::Approx(2.0f));
        CHECK(s.hdrMode == Config::HdrMode::Off);
        CHECK(s.hdrPeakNits == 0);
    }

    TEST_CASE("every existing and new key parses")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        WriteFile(path, R"(# full config
canvas_bg = 10,20,30
fade_delay_ms = 4200
fade_min_alpha = 0.35
filter_mode = bluenoise
pwm_bits = 1
window_width = 1024
window_height = 768
window_x = 120
window_y = 60
window_maximized = true
initial_zoom = 100
chrome_visible_on_start = false
wheel_action = next_prev
wheel_nav_threshold = 2.5
nav_zoom_mode = fit
ui_scale = 1.5
show_readout = false
show_control_strip = false
remember_window_size = false
hdr_mode = auto
hdr_transfer = pq
hdr_sdr_white_nits = 203
hdr_peak_nits = 1000
hdr_tone_map = false
)");

        const Config::Settings s = Config::Load(path.string(), false);

        CHECK(s.canvasBg[0] == 10);
        CHECK(s.canvasBg[1] == 20);
        CHECK(s.canvasBg[2] == 30);
        CHECK(s.fadeDelayMs == 4200);
        CHECK(s.fadeMinAlpha == doctest::Approx(0.35f));
        CHECK(s.filterMode == Config::kFilterModeBlueNoise);
        CHECK(s.pwmBits == 1u);
        CHECK(s.windowWidth == 1024);
        CHECK(s.windowHeight == 768);
        CHECK(s.windowX == 120);
        CHECK(s.windowY == 60);
        CHECK(s.windowMaximized);
        CHECK_FALSE(s.initialZoomFit);
        CHECK_FALSE(s.chromeVisible);
        CHECK(s.wheelAction == 1);
        CHECK(s.wheelNavThreshold == doctest::Approx(2.5f));
        CHECK(s.navZoomMode == 1);
        CHECK(s.uiScale == doctest::Approx(1.5f));
        CHECK_FALSE(s.showReadout);
        CHECK_FALSE(s.showControlStrip);
        CHECK_FALSE(s.rememberWindowSize);
        CHECK(s.hdrMode == Config::HdrMode::Auto);
        CHECK(s.hdrTransfer == Config::HdrTransfer::Pq);
        CHECK(s.hdrSdrWhiteNits == 203);
        CHECK(s.hdrPeakNits == 1000);
        CHECK_FALSE(s.hdrToneMap);
        CHECK(s.loadPath == path.string());
        CHECK(s.savePath == path.string());
    }

    TEST_CASE("invalid values fall back to defaults and do not fail load")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        WriteFile(path, R"(ui_scale = nope
pwm_bits = nope
fade_delay_ms = nope
fade_min_alpha = nope
filter_mode = bogus
hdr_mode = sometimes
hdr_transfer = rec709
hdr_sdr_white_nits = nope
hdr_peak_nits = nope
)");

        const Config::Settings s = Config::Load(path.string(), false);

        CHECK(s.uiScale == doctest::Approx(2.0f));
        CHECK(s.pwmBits == 1u);
        CHECK(s.fadeDelayMs == 1500);
        CHECK(s.fadeMinAlpha == doctest::Approx(0.15f));
        CHECK(s.filterMode == Config::kFilterModeNone);
        CHECK(s.hdrMode == Config::HdrMode::Off);
        CHECK(s.hdrTransfer == Config::HdrTransfer::ScRgb);
        CHECK(s.hdrSdrWhiteNits == 0);
        CHECK(s.hdrPeakNits == 0);
    }

    TEST_CASE("unknown keys are ignored")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        WriteFile(path, "totally_unknown = 42\n");

        const Config::Settings s = Config::Load(path.string(), false);

        CHECK(s.uiScale == doctest::Approx(2.0f));
        CHECK(s.filterMode == Config::kFilterModeNone);
        CHECK(s.hdrMode == Config::HdrMode::Off);
    }

    TEST_CASE("Save then Load preserves all fields")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        Config::Settings original = MakeNonDefault();
        original.savePath = path.string();

        REQUIRE(Config::Save(original));

        const Config::Settings loaded = Config::Load(path.string(), false);
        CheckValuesEqual(original, loaded);
        CHECK(loaded.loadPath == path.string());
        CHECK(loaded.savePath == path.string());
    }

    TEST_CASE("an unset window position is not written and stays unset")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        Config::Settings s;
        s.savePath = path.string();

        REQUIRE(Config::Save(s));

        const std::string text = ReadFile(path);
        CHECK(text.find("window_x") == std::string::npos);
        CHECK(text.find("window_y") == std::string::npos);
        CHECK(text.find("window_maximized = false") != std::string::npos);

        const Config::Settings loaded = Config::Load(path.string(), false);
        CHECK(loaded.windowX == Config::kWindowPositionUnset);
        CHECK(loaded.windowY == Config::kWindowPositionUnset);
    }

    TEST_CASE("half a window position sanitizes to no position")
    {
        Config::Settings s;
        s.windowX = 300;
        s.windowY = Config::kWindowPositionUnset;

        Config::Sanitize(s);

        CHECK(s.windowX == Config::kWindowPositionUnset);
        CHECK(s.windowY == Config::kWindowPositionUnset);

        s.windowX = -99999;
        s.windowY = 99999;
        Config::Sanitize(s);

        CHECK(s.windowX == -32768);
        CHECK(s.windowY == 32767);
    }

    TEST_CASE("all eight filter modes round-trip")
    {
        struct ModeCase
        {
            uint32_t value;
            const char* name;
        };
        const ModeCase cases[] = {
            { Config::kFilterModeNone,            "none" },
            { Config::kFilterModeOrdered,         "ordered" },
            { Config::kFilterModeFloyd,           "floyd" },
            { Config::kFilterModePwm,             "pwm" },
            { Config::kFilterModeBayer4x4,        "bayer4x4" },
            { Config::kFilterModeBayer8x8,        "bayer8x8" },
            { Config::kFilterModeBlueNoise,       "bluenoise" },
            { Config::kFilterModeSpatialTemporal, "spatial_temporal" },
        };

        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";

        for (const ModeCase& c : cases)
        {
            Config::Settings s;
            s.savePath = path.string();
            s.filterMode = c.value;
            s.pwmBits = 1;

            REQUIRE(Config::Save(s));
            const Config::Settings loaded = Config::Load(path.string(), false);
            CHECK(loaded.filterMode == c.value);
            const std::string text = ReadFile(path);
            CHECK(text.find("filter_mode = " + std::string(c.name)) != std::string::npos);
        }
    }

    TEST_CASE("filter mode constants match Args.hpp")
    {
        CHECK(Config::kFilterModeNone            == static_cast<uint32_t>(kModeNone));
        CHECK(Config::kFilterModeOrdered         == static_cast<uint32_t>(kModeOrdered));
        CHECK(Config::kFilterModeFloyd           == static_cast<uint32_t>(kModeFloydSteinberg));
        CHECK(Config::kFilterModePwm             == static_cast<uint32_t>(kModePWM));
        CHECK(Config::kFilterModeBayer4x4        == static_cast<uint32_t>(kModeBayer4x4));
        CHECK(Config::kFilterModeBayer8x8        == static_cast<uint32_t>(kModeBayer8x8));
        CHECK(Config::kFilterModeBlueNoise       == static_cast<uint32_t>(kModeBlueNoise));
        CHECK(Config::kFilterModeSpatialTemporal == static_cast<uint32_t>(kModeSpatialTemporal));
    }

    TEST_CASE("filter mode aliases are accepted")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";

        WriteFile(path, "filter_mode = floydsteinberg\n");
        CHECK(Config::Load(path.string(), false).filterMode == Config::kFilterModeFloyd);

        WriteFile(path, "filter_mode = spatial-temporal\n");
        CHECK(Config::Load(path.string(), false).filterMode == Config::kFilterModeSpatialTemporal);
    }

    TEST_CASE("Sanitize clamps ranges and falls back on unknown enums")
    {
        Config::Settings s;
        s.uiScale = 9.0f;
        s.pwmBits = 8;
        s.fadeDelayMs = -5;
        s.fadeMinAlpha = 2.0f;
        s.wheelNavThreshold = 0.0f;
        s.hdrSdrWhiteNits = 50;
        s.hdrPeakNits = 20000;
        s.filterMode = 42;
        s.wheelAction = 9;
        s.navZoomMode = 9;
        s.hdrMode = static_cast<Config::HdrMode>(99);
        s.hdrTransfer = static_cast<Config::HdrTransfer>(99);
        s.windowWidth = 1;
        s.windowHeight = 99999;
        s.canvasBg[0] = -1;
        s.canvasBg[1] = 300;
        s.canvasBg[2] = 42;

        Config::Sanitize(s);

        CHECK(s.uiScale == doctest::Approx(3.0f));
        CHECK(s.pwmBits == 1u);
        CHECK(s.fadeDelayMs == 0);
        CHECK(s.fadeMinAlpha == doctest::Approx(1.0f));
        CHECK(s.wheelNavThreshold == doctest::Approx(0.1f));
        CHECK(s.hdrSdrWhiteNits == 80);
        CHECK(s.hdrPeakNits == 10000);
        CHECK(s.filterMode == Config::kFilterModeNone);
        CHECK(s.wheelAction == 0);
        CHECK(s.navZoomMode == 0);
        CHECK(s.hdrMode == Config::HdrMode::Off);
        CHECK(s.hdrTransfer == Config::HdrTransfer::ScRgb);
        CHECK(s.windowWidth == 64);
        CHECK(s.windowHeight == 16384);
        CHECK(s.canvasBg[0] == 0);
        CHECK(s.canvasBg[1] == 255);
        CHECK(s.canvasBg[2] == 42);
    }

    TEST_CASE("Sanitize handles lower bounds and NaN")
    {
        Config::Settings s;
        s.uiScale = 0.5f;
        s.pwmBits = 0;
        s.fadeDelayMs = 100000;
        s.fadeMinAlpha = -1.0f;
        s.wheelNavThreshold = 1000.0f;
        s.hdrSdrWhiteNits = 2000;
        s.hdrPeakNits = 50;
        s.windowWidth = 100000;
        s.windowHeight = 1;

        Config::Sanitize(s);

        CHECK(s.uiScale == doctest::Approx(1.0f));
        CHECK(s.pwmBits == 1u);
        CHECK(s.fadeDelayMs == 60000);
        CHECK(s.fadeMinAlpha == doctest::Approx(0.0f));
        CHECK(s.wheelNavThreshold == doctest::Approx(100.0f));
        CHECK(s.hdrSdrWhiteNits == 1000);
        CHECK(s.hdrPeakNits == 100);
        CHECK(s.windowWidth == 16384);
        CHECK(s.windowHeight == 64);

        s.uiScale = std::numeric_limits<float>::quiet_NaN();
        s.fadeMinAlpha = std::numeric_limits<float>::quiet_NaN();
        s.wheelNavThreshold = std::numeric_limits<float>::quiet_NaN();

        Config::Sanitize(s);

        CHECK(s.uiScale == doctest::Approx(2.0f));
        CHECK(s.fadeMinAlpha == doctest::Approx(0.15f));
        CHECK(s.wheelNavThreshold == doctest::Approx(1.0f));
    }

    TEST_CASE("Sanitize preserves valid boundary values")
    {
        Config::Settings s;
        s.uiScale = 1.0f;
        s.fadeMinAlpha = 0.0f;
        s.wheelNavThreshold = 0.1f;
        s.hdrSdrWhiteNits = 80;
        s.hdrPeakNits = 100;
        s.pwmBits = 1;
        s.filterMode = Config::kFilterModeSpatialTemporal;
        s.hdrMode = Config::HdrMode::Force;
        s.hdrTransfer = Config::HdrTransfer::Pq;

        Config::Sanitize(s);

        CHECK(s.uiScale == doctest::Approx(1.0f));
        CHECK(s.fadeMinAlpha == doctest::Approx(0.0f));
        CHECK(s.wheelNavThreshold == doctest::Approx(0.1f));
        CHECK(s.hdrSdrWhiteNits == 80);
        CHECK(s.hdrPeakNits == 100);
        CHECK(s.pwmBits == 1u);
        CHECK(s.filterMode == Config::kFilterModeSpatialTemporal);
        CHECK(s.hdrMode == Config::HdrMode::Force);
        CHECK(s.hdrTransfer == Config::HdrTransfer::Pq);
    }

    TEST_CASE("Load applies Sanitize")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        WriteFile(path, R"(ui_scale = 9.0
pwm_bits = 8
fade_delay_ms = 99999
fade_min_alpha = 2.0
hdr_sdr_white_nits = 50
hdr_peak_nits = 20000
filter_mode = spatial_temporal
)");

        const Config::Settings s = Config::Load(path.string(), false);

        CHECK(s.uiScale == doctest::Approx(3.0f));
        CHECK(s.pwmBits == 1u);
        CHECK(s.fadeDelayMs == 60000);
        CHECK(s.fadeMinAlpha == doctest::Approx(1.0f));
        CHECK(s.hdrSdrWhiteNits == 80);
        CHECK(s.hdrPeakNits == 10000);
        CHECK(s.filterMode == Config::kFilterModeSpatialTemporal);
    }

    TEST_CASE("Save writes sanitized values")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        Config::Settings s;
        s.savePath = path.string();
        s.uiScale = 9.0f;
        s.pwmBits = 8;
        s.hdrSdrWhiteNits = 50;

        REQUIRE(Config::Save(s));

        const Config::Settings loaded = Config::Load(path.string(), false);
        CHECK(loaded.uiScale == doctest::Approx(3.0f));
        CHECK(loaded.pwmBits == 1u);
        CHECK(loaded.hdrSdrWhiteNits == 80);

        const std::string text = ReadFile(path);
        CHECK(text.find("pwm_bits = 1") != std::string::npos);
    }

    TEST_CASE("Save is atomic and leaves the old file intact on failure")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "iview.conf";
        Config::Settings original = MakeNonDefault();
        original.savePath = path.string();

        REQUIRE(Config::Save(original));
        const std::string before = ReadFile(path);

        /* ./iview.conf.tmp as a directory makes the temp-file open fail. */
        std::filesystem::path blocker = path;
        blocker += ".tmp";
        std::error_code ec;
        REQUIRE(std::filesystem::create_directory(blocker, ec));
        REQUIRE_FALSE(ec);

        Config::Settings changed = original;
        changed.filterMode = Config::kFilterModeBlueNoise;
        CHECK_FALSE(Config::Save(changed));
        CHECK(ReadFile(path) == before);
        CHECK(std::filesystem::is_directory(blocker));
        CHECK(Config::Load(path.string(), false).filterMode == original.filterMode);

        std::filesystem::remove(blocker, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(Config::Save(changed));
        CHECK(Config::Load(path.string(), false).filterMode == Config::kFilterModeBlueNoise);
    }

    TEST_CASE("Save creates missing parent directories")
    {
        TempDir dir;
        const std::filesystem::path path = dir.path / "nested" / "deeper" / "iview.conf";
        Config::Settings s;
        s.savePath = path.string();

        REQUIRE(Config::Save(s));
        CHECK(std::filesystem::is_regular_file(path));
    }

    TEST_CASE("Save fails cleanly for an empty path or a file parent")
    {
        Config::Settings empty;
        CHECK_FALSE(Config::Save(empty));

        TempDir dir;
        const std::filesystem::path parentFile = dir.path / "not_a_dir";
        WriteFile(parentFile, "x");
        Config::Settings bad;
        bad.savePath = (parentFile / "iview.conf").string();
        CHECK_FALSE(Config::Save(bad));
    }
}
