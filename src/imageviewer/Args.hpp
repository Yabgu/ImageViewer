#pragma once
/*
 * Args.hpp — command-line parsing for the viewer.
 *
 * Deliberately pure: no I/O, no globals, no exceptions, and no dependency on
 * GLFW or the module system, so it can be unit-tested from iview_tests without
 * a display.  Errors are reported through ViewerArgs::parseError instead of
 * being thrown, because every failure here is an ordinary user mistake rather
 * than an exceptional condition.
 */

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

/* ── Filter modes ────────────────────────────────────────────────────────────
 *
 * IWFilterMode mirrors the filter plugin's internal mode constants, which are
 * passed to it as the named uint32 argument "mode" (see FilterPluginDef.h and
 * FilterPluginPixman.ixx).  The plugin keeps those values private to itself, so
 * the host has to know the numbers; the mapping is pinned by the ABI suite
 * (iview_abi_tests), which drives the real plugin with these exact values and
 * asserts the resulting behaviour.  If the plugin ever renumbers them, that
 * test fails rather than the CLI silently selecting the wrong mode.
 */
enum IWFilterMode : uint32_t {
    kModeNone           = 0u,   /* pass-through conversion, no dithering     */
    kModeOrdered        = 1u,   /* 4x4 Bayer ordered dither                  */
    kModeFloydSteinberg = 2u,   /* Floyd-Steinberg error diffusion           */
    kModePWM            = 3u    /* temporal / PWM dither, multiple frames    */
};

/* ── pwm_bits range ──────────────────────────────────────────────────────────
 *
 * The cap is enforced by the plugin, not here: FilterCore::ApplyPWM() clamps
 * with `if (pwmBits == 0u) pwmBits = 1u; if (pwmBits > 8u) pwmBits = 8u;`
 * ("cap at 256 frames"), and FilterPluginPixman.ixx documents pwm_bits as
 * "extra virtual bits for PWM mode (default 1, max 8)".  Rejecting out-of-range
 * values in the parser rather than letting the plugin clamp them keeps a typo
 * from silently producing 256 frames of a 12 MP image.
 */
constexpr uint32_t kViewerMinPwmBits = 1u;
constexpr uint32_t kViewerMaxPwmBits = 8u;

/* ── Parsed result ─────────────────────────────────────────────────────────*/

struct ViewerArgs
{
    std::string  imagePath;        /* empty only when helpRequested          */
    IWFilterMode mode        = kModeNone;
    uint32_t     pwmBits     = 0u; /* 0 = do not send pwm_bits, use plugin default */
    bool         helpRequested = false;
    std::string  parseError;       /* empty on success                       */
};

/* ── Usage text ────────────────────────────────────────────────────────────*/

inline const char* UsageText() noexcept
{
    return
        "Usage: iview [options] <image>\n"
        "\n"
        "Options:\n"
        "  --filter=MODE   Dithering mode: none (default), ordered, floyd, pwm\n"
        "  --pwm-bits=N    Extra virtual bits for --filter=pwm; N in [1, 8]\n"
        "  --help          Show this help and exit\n"
        "  --              Treat every remaining argument as a file name\n"
        "\n"
        "Examples:\n"
        "  iview photo.jpg\n"
        "  iview --filter=pwm --pwm-bits=2 photo.jpg\n"
        "  iview --filter=ordered photo.jpg\n";
}

namespace args_detail {

inline bool ParseMode(const std::string& value, IWFilterMode& out) noexcept
{
    if (value == "none")    { out = kModeNone;           return true; }
    if (value == "ordered") { out = kModeOrdered;        return true; }
    if (value == "floyd")   { out = kModeFloydSteinberg; return true; }
    if (value == "pwm")     { out = kModePWM;            return true; }
    return false;
}

/*
 * Strict unsigned decimal parse: digits only, no sign, no whitespace, and an
 * overflow guard.  "-1" and "3x" are both rejected, which is what makes the
 * single range error below cover negative and non-numeric input too.
 */
inline bool ParseU32(const std::string& value, uint32_t& out) noexcept
{
    if (value.empty() || value.size() > 10u)
        return false;
    uint64_t acc = 0;
    for (char c : value) {
        if (c < '0' || c > '9')
            return false;
        acc = acc * 10u + static_cast<uint64_t>(c - '0');
        if (acc > 0xFFFFFFFFull)
            return false;
    }
    out = static_cast<uint32_t>(acc);
    return true;
}

} // namespace args_detail

/* ── Parser ────────────────────────────────────────────────────────────────*/

inline ViewerArgs ParseViewerArgs(int argc, const char* const* argv)
{
    ViewerArgs out;
    bool afterDoubleDash = false;
    bool sawPwmBits      = false;

    /* --help wins regardless of position, so someone who mistypes another flag
     * still gets the usage text.  The scan stops at "--", because everything
     * after that delimiter is a file name and "--help" there is not a flag. */
    for (int i = 1; i < argc; ++i) {
        const std::string probe = (argv[i] != nullptr) ? argv[i] : std::string();
        if (probe == "--") break;
        if (probe == "--help") {
            out.helpRequested = true;
            return out;
        }
    }

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = (argv[i] != nullptr) ? argv[i] : std::string();

        if (afterDoubleDash) {
            if (!out.imagePath.empty()) {
                out.parseError = "more than one image path given: '" +
                                 out.imagePath + "' and '" + arg + "'";
                return out;
            }
            out.imagePath = arg;
            continue;
        }

        if (arg == "--") { afterDoubleDash = true; continue; }

        if (arg == "--help") {
            /* Help wins: stop parsing so `iview --help` needs no image. */
            out.helpRequested = true;
            out.parseError.clear();
            return out;
        }

        if (arg.rfind("--filter=", 0) == 0) {
            const std::string value = arg.substr(9);
            if (!args_detail::ParseMode(value, out.mode)) {
                out.parseError = "unknown filter mode: '" + value +
                                 "' (expected none, ordered, floyd or pwm)";
                return out;
            }
            continue;
        }

        if (arg.rfind("--pwm-bits=", 0) == 0) {
            const std::string value = arg.substr(11);
            uint32_t parsed = 0;
            if (!args_detail::ParseU32(value, parsed) ||
                parsed < kViewerMinPwmBits || parsed > kViewerMaxPwmBits) {
                out.parseError = "--pwm-bits must be an integer in [1, 8]";
                return out;
            }
            out.pwmBits  = parsed;
            sawPwmBits   = true;
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            out.parseError = "unknown option: " + arg;
            return out;
        }

        if (!out.imagePath.empty()) {
            out.parseError = "more than one image path given: '" +
                             out.imagePath + "' and '" + arg + "'";
            return out;
        }
        out.imagePath = arg;
    }

    /* --pwm-bits is only meaningful for the PWM mode; checked after the loop so
     * that flag order does not matter (`--pwm-bits=2 --filter=pwm` is valid). */
    if (sawPwmBits && out.mode != kModePWM) {
        out.parseError = "--pwm-bits is only valid with --filter=pwm";
        return out;
    }

    if (out.imagePath.empty()) {
        out.parseError = "no image path given";
        return out;
    }

    return out;
}

/* ── Wide-argument helper ──────────────────────────────────────────────────*/

/*
 * Convert a wchar_t argv (Windows wWinMain via CommandLineToArgvW) to narrow
 * UTF-8 strings so the single char-based parser above serves both platforms.
 *
 * This exists so the Windows entry point does not need its own parser.  It is
 * portable code rather than a WideCharToMultiByte call so that the conversion
 * can be unit-tested on Linux.
 */
inline std::vector<std::string> NarrowArgv(int argc, const wchar_t* const* argv)
{
    std::vector<std::string> out;
    out.reserve(argc > 0 ? static_cast<size_t>(argc) : 0u);

    for (int i = 0; i < argc; ++i)
    {
        const wchar_t* w = (argv != nullptr) ? argv[i] : nullptr;
        std::string s;
        if (w != nullptr) {
            while (*w != L'\0') {
                uint32_t cp = static_cast<uint32_t>(*w++);

                /* Combine UTF-16 surrogate pairs into one code point. */
                if (cp >= 0xD800u && cp <= 0xDBFFu && *w != L'\0') {
                    const uint32_t low = static_cast<uint32_t>(*w);
                    if (low >= 0xDC00u && low <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        ++w;
                    } else {
                        cp = 0xFFFDu; /* unpaired high surrogate */
                    }
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    cp = 0xFFFDu;     /* unpaired low surrogate */
                }

                if (cp < 0x80u) {
                    s.push_back(static_cast<char>(cp));
                } else if (cp < 0x800u) {
                    s.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                    s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else if (cp < 0x10000u) {
                    s.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                    s.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                    s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else {
                    s.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                    s.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                    s.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                    s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                }
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

/*
 * Parse argv into a single char-based form.  Windows passes UTF-16 through
 * wWinMain, so the wide array is narrowed to UTF-8 first; everything after that
 * is one code path, which is why ParseViewerArgs() above is platform-neutral.
 *
 * Lives here rather than in main_generic.hpp so the wchar_t instantiation can
 * be compiled and tested without the GUI module stack.
 */
template <typename T>
inline ViewerArgs ParseArgs(int argc, T* argv[])
{
    std::vector<std::string> store;
    if constexpr (std::is_same_v<T, wchar_t>) {
        store = NarrowArgv(argc, argv);
    } else {
        store.reserve(argc > 0 ? static_cast<size_t>(argc) : 0u);
        for (int i = 0; i < argc; ++i)
            store.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }

    std::vector<const char*> ptrs;
    ptrs.reserve(store.size());
    for (const std::string& s : store)
        ptrs.push_back(s.c_str());

    return ParseViewerArgs(static_cast<int>(ptrs.size()),
                           ptrs.empty() ? nullptr : ptrs.data());
}

/*
 * Build a filesystem path from the parsed UTF-8 image path.
 *
 * On Windows a path constructed from a narrow string is interpreted in the
 * active ANSI code page, not UTF-8, so non-ASCII file names would be mangled.
 * Constructing from u8string is the explicit-UTF-8 form.  On POSIX the bytes are
 * passed through unchanged, as required.
 */
inline std::filesystem::path PathFromUtf8(const std::string& s)
{
#ifdef _WIN32
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::path(s);
#endif
}
