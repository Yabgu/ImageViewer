/*
 * test_imageloader_memory.cpp — the memory-span contract, across every loader.
 *
 * The per-format files (test_imageloader_{png,webp,tiff,stb,iwi}.cpp, and the
 * JPEG file that predates them) each test one loaders's own descriptor policy.  This
 * file tests the property that is common to all of them and that a host reading
 * assets out of a container depends on:
 *
 *   1. LoadImageFromMemory(bytes) == LoadImageFromFile(path) for the same bytes
 *      -- same geometry, same IWImageFormat, same pixels, for every loader.
 *   2. Empty and null spans are refused with a code, never dereferenced.
 *   3. A loader that does not export the symbol is file-only, not broken.
 *
 * (3) is asserted against the resolution mechanism the host actually uses:
 * an optional symbol that does not resolve yields null, and no load failure.
 * PluginManager takes exactly this path (dlsym/GetProcAddress, absence is not
 * an error), but it is a C++23 module target and this harness is a C++17
 * binary, so the assertion is made at the layer both share: SharedLibrary.
 */

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "plugin_abi.hpp"

#ifndef IW_ABI_FIXTURE_DIR
#  error "IW_ABI_FIXTURE_DIR must be defined by the build system"
#endif

namespace {

std::filesystem::path Fixture(const char* name)
{
    return std::filesystem::path{ IW_ABI_FIXTURE_DIR } / name;
}

/*
 * Decode the same bytes both ways and return the difference (empty on match).
 * Frees both results, so a difference is reported as a string rather than as a
 * leaked pair of buffers under a failed REQUIRE.
 */
std::string CompareBothPaths(const std::filesystem::path& plugin,
                             const std::filesystem::path& image,
                             ImagePluginResultCode* fileCode,
                             ImagePluginResultCode* memoryCode)
{
    iwabi::ImageLoaderPlugin loader{ plugin };
    if (!loader.hasMemory()) return "plugin has no LoadImageFromMemory export";

    const std::vector<uint8_t> bytes = iwabi::ReadFileBytes(image);
    if (bytes.empty()) return "fixture is empty";

    ImagePluginResult fromFile   = loader.load(image);
    ImagePluginResult fromMemory = loader.loadMemory(bytes.data(), bytes.size());

    if (fileCode)   *fileCode   = fromFile.code;
    if (memoryCode) *memoryCode = fromMemory.code;

    std::string difference;
    if (fromFile.code != IMAGE_PLUGIN_OK || fromMemory.code != IMAGE_PLUGIN_OK)
        difference = "a decode failed";
    else if (!fromFile.data || !fromMemory.data)
        difference = "a decode returned no data";
    else
        difference = iwabi::DecodeDifference(*fromFile.data, *fromMemory.data);

    loader.free(fromFile.data);
    loader.free(fromMemory.data);
    return difference;
}

/* Every loader must refuse these two the same way.  Null data with a non-zero
 * size is the case that used to be indistinguishable from a null path. */
void CheckRefusals(const std::filesystem::path& plugin)
{
    iwabi::ImageLoaderPlugin loader{ plugin };
    REQUIRE(loader.hasMemory());

    ImagePluginResult empty = loader.loadMemory(nullptr, 0);
    CHECK(empty.code == IMAGE_PLUGIN_FILE_ERROR);
    CHECK(empty.data == nullptr);

    ImagePluginResult nullData = loader.loadMemory(nullptr, 4096);
    CHECK(nullData.code == IMAGE_PLUGIN_FILE_ERROR);
    CHECK(nullData.data == nullptr);
}

} // namespace

TEST_SUITE("memory-span contract: file and memory agree")
{
#if defined(IW_JPEG_PLUGIN_PATH)
    TEST_CASE("JPEG: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_JPEG_PLUGIN_PATH }),
            Fixture("ycbcr_3comp.jpg"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif

#if defined(IW_PNG_PLUGIN_PATH)
    TEST_CASE("PNG: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_PNG_PLUGIN_PATH }),
            Fixture("rgba_4x4.png"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif

#if defined(IW_IWI_PLUGIN_PATH)
    TEST_CASE("IWI: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_IWI_PLUGIN_PATH }),
            Fixture("rgba_4x4.iwi"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif

#if defined(IW_WEBP_PLUGIN_PATH)
    TEST_CASE("WebP: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_WEBP_PLUGIN_PATH }),
            Fixture("lossless_4x4.webp"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif

#if defined(IW_TIFF_PLUGIN_PATH)
    TEST_CASE("TIFF: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_TIFF_PLUGIN_PATH }),
            Fixture("rgb_4x4.tif"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif

#if defined(IW_GIF_PLUGIN_PATH)
    TEST_CASE("GIF: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_GIF_PLUGIN_PATH }),
            Fixture("gif_4x4.gif"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif

#if defined(IW_STB_PLUGIN_PATH)
    TEST_CASE("stb: memory decode equals file decode")
    {
        ImagePluginResultCode f = IMAGE_PLUGIN_UNKNOWN_ERROR, m = IMAGE_PLUGIN_UNKNOWN_ERROR;
        const std::string difference = CompareBothPaths(
            iwabi::DeployedPluginPath(std::filesystem::path{ IW_STB_PLUGIN_PATH }),
            Fixture("rgb_4x4.bmp"), &f, &m);
        INFO("file code " << static_cast<int>(f) << ", memory code " << static_cast<int>(m));
        CHECK(difference.empty());
    }
#endif
}

TEST_SUITE("memory-span contract: refusals")
{
#if defined(IW_JPEG_PLUGIN_PATH)
    TEST_CASE("JPEG refuses empty and null spans")
    {
        CheckRefusals(iwabi::DeployedPluginPath(std::filesystem::path{ IW_JPEG_PLUGIN_PATH }));
    }
#endif
#if defined(IW_PNG_PLUGIN_PATH)
    TEST_CASE("PNG refuses empty and null spans")
    {
        CheckRefusals(iwabi::DeployedPluginPath(std::filesystem::path{ IW_PNG_PLUGIN_PATH }));
    }
#endif
#if defined(IW_WEBP_PLUGIN_PATH)
    TEST_CASE("WebP refuses empty and null spans")
    {
        CheckRefusals(iwabi::DeployedPluginPath(std::filesystem::path{ IW_WEBP_PLUGIN_PATH }));
    }
#endif
#if defined(IW_TIFF_PLUGIN_PATH)
    TEST_CASE("TIFF refuses empty and null spans")
    {
        CheckRefusals(iwabi::DeployedPluginPath(std::filesystem::path{ IW_TIFF_PLUGIN_PATH }));
    }
#endif
#if defined(IW_GIF_PLUGIN_PATH)
    TEST_CASE("GIF refuses empty and null spans")
    {
        CheckRefusals(iwabi::DeployedPluginPath(std::filesystem::path{ IW_GIF_PLUGIN_PATH }));
    }
#endif
#if defined(IW_STB_PLUGIN_PATH)
    TEST_CASE("stb refuses empty and null spans")
    {
        CheckRefusals(iwabi::DeployedPluginPath(std::filesystem::path{ IW_STB_PLUGIN_PATH }));
    }
#endif
}

TEST_SUITE("memory-span contract: optional export")
{
    TEST_CASE("An absent optional export resolves to null and is not a load failure")
    {
        /* The plugin loads; only the optional symbol is missing.  This is the
         * state of a loader built before the memory path existed, and the host's
         * contract is to fall back to the file entry point rather than to fail
         * the plugin. */
#if defined(IW_JPEG_PLUGIN_PATH)
        iwabi::SharedLibrary lib{ iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_JPEG_PLUGIN_PATH }) };

        CHECK(lib.trySymbol("LoadImageFromFile") != nullptr);
        CHECK(lib.trySymbol("LoadImageFromMemory") != nullptr);
        CHECK(lib.trySymbol("LoadImageFromMemoryDeprecated") == nullptr);
#else
        MESSAGE("no loader available in this build; nothing to resolve");
#endif
    }

    TEST_CASE("Every built loader exports the memory entry point")
    {
        int checked = 0;
#if defined(IW_JPEG_PLUGIN_PATH)
        CHECK(iwabi::ImageLoaderPlugin(iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_JPEG_PLUGIN_PATH })).hasMemory());
        ++checked;
#endif
#if defined(IW_PNG_PLUGIN_PATH)
        CHECK(iwabi::ImageLoaderPlugin(iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_PNG_PLUGIN_PATH })).hasMemory());
        ++checked;
#endif
#if defined(IW_WEBP_PLUGIN_PATH)
        CHECK(iwabi::ImageLoaderPlugin(iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_WEBP_PLUGIN_PATH })).hasMemory());
        ++checked;
#endif
#if defined(IW_TIFF_PLUGIN_PATH)
        CHECK(iwabi::ImageLoaderPlugin(iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_TIFF_PLUGIN_PATH })).hasMemory());
        ++checked;
#endif
#if defined(IW_GIF_PLUGIN_PATH)
        CHECK(iwabi::ImageLoaderPlugin(iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_GIF_PLUGIN_PATH })).hasMemory());
        ++checked;
#endif
#if defined(IW_STB_PLUGIN_PATH)
        CHECK(iwabi::ImageLoaderPlugin(iwabi::DeployedPluginPath(
            std::filesystem::path{ IW_STB_PLUGIN_PATH })).hasMemory());
        ++checked;
#endif
        INFO("loaders checked: " << checked);
        CHECK(checked > 0);
    }
}
