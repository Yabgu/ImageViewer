#pragma once
/* ── Config ──────────────────────────────────────────────────────────────────
 *
 * Flat key=value settings, no dependencies (std::ifstream + getline).
 *
 * Load order (first hit wins):
 *   1. --config=<path>
 *   2. %APPDATA%/ImageViewer/iview.conf      (XDG_CONFIG_HOME elsewhere)
 *   3. <exe_dir>/iview.conf
 *   4. builtin defaults
 *
 * Save always goes to the --config path when one was given, otherwise to the
 * %APPDATA% location (the directory is created on demand).  The write is
 * atomic: a sibling .tmp file is written, flushed and renamed over the target.
 * An unchanged file is not rewritten.
 *
 * Sanitize() is the single range-clamping entry point.  Load() always calls it
 * after reading a file, and UI apply paths must call it before persisting.
 *
 * Unknown keys warn to stderr and are ignored; an unparseable value falls back
 * to the builtin default with one warning.  A broken config never fails start-up.
 */

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace Config {

enum class HdrMode : uint32_t
{
	Off   = 0,
	Auto  = 1,
	Force = 2
};

enum class HdrTransfer : uint32_t
{
	ScRgb = 0,
	Pq    = 1
};

/* These numeric values must match IWFilterMode in Args.hpp.  The Config tests
 * pin the mapping, so a renumbering in one place fails the suite rather than
 * silently loading the wrong filter. */
inline constexpr uint32_t kFilterModeNone            = 0u;
inline constexpr uint32_t kFilterModeOrdered         = 1u;
inline constexpr uint32_t kFilterModeFloyd           = 2u;
inline constexpr uint32_t kFilterModePwm             = 3u;
inline constexpr uint32_t kFilterModeBayer4x4        = 4u;
inline constexpr uint32_t kFilterModeBayer8x8        = 5u;
inline constexpr uint32_t kFilterModeBlueNoise       = 6u;
inline constexpr uint32_t kFilterModeSpatialTemporal = 7u;

inline constexpr int   kDefaultFadeDelayMs       = 1500;
inline constexpr float kDefaultFadeMinAlpha      = 0.15f;
inline constexpr float kDefaultWheelNavThreshold = 1.0f;
inline constexpr float kDefaultUiScale           = 2.0f;

inline constexpr float kMinUiScale = 1.0f;
inline constexpr float kMaxUiScale = 3.0f;
inline constexpr int   kMaxFadeDelayMs = 60000;

/* window_x/window_y sentinel: "no position was saved yet", which makes the
 * viewer centre the window instead of reusing a stale spot.  0 is a valid
 * coordinate on multi-monitor desktops, so it cannot double as "unset". */
inline constexpr int   kWindowPositionUnset = std::numeric_limits<int>::min();

inline constexpr int kMinHdrSdrWhiteNits = 80;
inline constexpr int kMaxHdrSdrWhiteNits = 1000;
inline constexpr int kMinHdrPeakNits     = 100;
inline constexpr int kMaxHdrPeakNits     = 10000;

struct Settings
{
	int         canvasBg[3]   = { 128, 128, 128 };
	int         fadeDelayMs   = kDefaultFadeDelayMs;
	float       fadeMinAlpha  = kDefaultFadeMinAlpha;
	uint32_t    filterMode    = kFilterModeNone; /* IWFilterMode: 0..7 */
	uint32_t    pwmBits       = 1;               /* capped at 1 for now */
	int         windowWidth   = 1280;
	int         windowHeight  = 720;
	/* Client-area origin in screen coordinates (GLFW's convention).  Kept with
	 * the size so a restart lands where the window was left; kWindowPositionUnset
	 * means "no position saved" and the window is centred instead. */
	int         windowX       = kWindowPositionUnset;
	int         windowY       = kWindowPositionUnset;
	bool        windowMaximized = false;
	bool        initialZoomFit = true;  /* false = 100 %                    */
	bool        chromeVisible = true;
	int         wheelAction   = 0;      /* 0 = zoom, 1 = prev/next           */
	float       wheelNavThreshold = kDefaultWheelNavThreshold;
	int         navZoomMode   = 0;      /* 0 = preserve, 1 = fit, 2 = 100 %  */

	/* Interface */
	float       uiScale           = kDefaultUiScale;
	bool        showReadout       = true;
	bool        showControlStrip  = true;
	bool        rememberWindowSize = true;

	/* HDR: parsed and saved now, but deliberately has no renderer effect yet. */
	HdrMode     hdrMode         = HdrMode::Off;
	HdrTransfer hdrTransfer     = HdrTransfer::ScRgb;
	int         hdrSdrWhiteNits = 0;    /* 0 = auto */
	int         hdrPeakNits     = 0;    /* 0 = auto */
	bool        hdrToneMap      = true;

	/* Filled by Load(); not written by Save(). */
	std::string loadPath;               /* empty when only defaults applied */
	std::string savePath;               /* where Save() writes              */
};

inline void SetDefaults(Settings& s)
{
	s = Settings{};
}

inline std::string Trim(std::string s)
{
	const char* const ws = " \t\r\n";
	const auto first = s.find_first_not_of(ws);
	if (first == std::string::npos) return {};
	const auto last = s.find_last_not_of(ws);
	return s.substr(first, last - first + 1);
}

inline bool ParseInt(const std::string& text, int& out)
{
	if (text.empty()) return false;
	char* end = nullptr;
	const long v = std::strtol(text.c_str(), &end, 10);
	if (end == nullptr || *end != '\0' || v < -1000000 || v > 1000000) return false;
	out = static_cast<int>(v);
	return true;
}

inline bool ParseU32(const std::string& text, uint32_t& out)
{
	if (text.empty()) return false;
	char* end = nullptr;
	errno = 0;
	const unsigned long v = std::strtoul(text.c_str(), &end, 10);
	if (end == nullptr || *end != '\0' || errno == ERANGE ||
	    v > std::numeric_limits<uint32_t>::max()) return false;
	out = static_cast<uint32_t>(v);
	return true;
}

inline bool ParseFloat(const std::string& text, float& out)
{
	if (text.empty()) return false;
	char* end = nullptr;
	const double v = std::strtod(text.c_str(), &end);
	if (end == nullptr || *end != '\0') return false;
	out = static_cast<float>(v);
	return true;
}

inline bool ParseBool(const std::string& text, bool& out)
{
	if (text == "true" || text == "1" || text == "yes" || text == "on")  { out = true;  return true; }
	if (text == "false" || text == "0" || text == "no" || text == "off") { out = false; return true; }
	return false;
}

/* "128,64,32" -> three ints in [0,255]. */
inline bool ParseRgb(const std::string& text, int (&out)[3])
{
	int values[3] = {0, 0, 0};
	size_t start = 0;
	for (int i = 0; i < 3; ++i)
	{
		const size_t comma = text.find(',', start);
		const std::string part = Trim(text.substr(start, (comma == std::string::npos) ? std::string::npos : comma - start));
		if (!ParseInt(part, values[i]) || values[i] < 0 || values[i] > 255) return false;
		if (i < 2)
		{
			if (comma == std::string::npos) return false;
			start = comma + 1;
		}
		else if (comma != std::string::npos)
		{
			return false;   /* trailing garbage */
		}
	}
	out[0] = values[0]; out[1] = values[1]; out[2] = values[2];
	return true;
}

/* Canonical names written by Save().  ParseFilterMode() also accepts the two
 * aliases Args.hpp accepts. */
inline const char* FilterModeName(uint32_t mode) noexcept
{
	switch (mode)
	{
	case kFilterModeNone:            return "none";
	case kFilterModeOrdered:         return "ordered";
	case kFilterModeFloyd:           return "floyd";
	case kFilterModePwm:             return "pwm";
	case kFilterModeBayer4x4:        return "bayer4x4";
	case kFilterModeBayer8x8:        return "bayer8x8";
	case kFilterModeBlueNoise:       return "bluenoise";
	case kFilterModeSpatialTemporal: return "spatial_temporal";
	default:                         return nullptr;
	}
}

inline bool ParseFilterMode(const std::string& text, uint32_t& out) noexcept
{
	if (text == "none")             { out = kFilterModeNone;            return true; }
	if (text == "ordered")          { out = kFilterModeOrdered;         return true; }
	if (text == "floyd" ||
	    text == "floydsteinberg")   { out = kFilterModeFloyd;           return true; }
	if (text == "pwm")              { out = kFilterModePwm;             return true; }
	if (text == "bayer4x4")         { out = kFilterModeBayer4x4;        return true; }
	if (text == "bayer8x8")         { out = kFilterModeBayer8x8;        return true; }
	if (text == "bluenoise")        { out = kFilterModeBlueNoise;       return true; }
	if (text == "spatial_temporal" ||
	    text == "spatial-temporal") { out = kFilterModeSpatialTemporal; return true; }
	return false;
}

inline const char* HdrModeName(HdrMode mode) noexcept
{
	switch (mode)
	{
	case HdrMode::Off:   return "off";
	case HdrMode::Auto:  return "auto";
	case HdrMode::Force: return "force";
	default:             return nullptr;
	}
}

inline bool ParseHdrMode(const std::string& text, HdrMode& out) noexcept
{
	if (text == "off")   { out = HdrMode::Off;   return true; }
	if (text == "auto")  { out = HdrMode::Auto;  return true; }
	if (text == "force") { out = HdrMode::Force; return true; }
	return false;
}

inline const char* HdrTransferName(HdrTransfer transfer) noexcept
{
	switch (transfer)
	{
	case HdrTransfer::ScRgb: return "scrgb";
	case HdrTransfer::Pq:    return "pq";
	default:                 return nullptr;
	}
}

inline bool ParseHdrTransfer(const std::string& text, HdrTransfer& out) noexcept
{
	if (text == "scrgb") { out = HdrTransfer::ScRgb; return true; }
	if (text == "pq")    { out = HdrTransfer::Pq;    return true; }
	return false;
}

/* Single clamping entry point.  Called from Load() and from any UI apply path
 * before values are persisted. */
inline void Sanitize(Settings& s)
{
	for (int& c : s.canvasBg)
		c = std::clamp(c, 0, 255);

	s.fadeDelayMs = std::clamp(s.fadeDelayMs, 0, kMaxFadeDelayMs);
	if (std::isnan(s.fadeMinAlpha)) s.fadeMinAlpha = kDefaultFadeMinAlpha;
	s.fadeMinAlpha = std::clamp(s.fadeMinAlpha, 0.0f, 1.0f);

	switch (s.filterMode)
	{
	case kFilterModeNone:
	case kFilterModeOrdered:
	case kFilterModeFloyd:
	case kFilterModePwm:
	case kFilterModeBayer4x4:
	case kFilterModeBayer8x8:
	case kFilterModeBlueNoise:
	case kFilterModeSpatialTemporal:
		break;
	default:
		s.filterMode = kFilterModeNone;
		break;
	}

	/* TODO(pwm): main_generic currently rejects N-phase output (>1).  Keep the
	 * two paths in lockstep by capping here; raise this together with the
	 * renderer's N-phase support, not before. */
	s.pwmBits = 1u;

	s.windowWidth  = std::clamp(s.windowWidth, 64, 16384);
	s.windowHeight = std::clamp(s.windowHeight, 64, 16384);

	/* Half a position is no position: never place a window at (x,0) just because
	 * one of the two keys was missing.  The range keeps every plausible
	 * multi-monitor layout while rejecting a corrupted value outright. */
	if (s.windowX == kWindowPositionUnset || s.windowY == kWindowPositionUnset)
	{
		s.windowX = kWindowPositionUnset;
		s.windowY = kWindowPositionUnset;
	}
	else
	{
		s.windowX = std::clamp(s.windowX, -32768, 32767);
		s.windowY = std::clamp(s.windowY, -32768, 32767);
	}

	if (s.wheelAction != 0 && s.wheelAction != 1) s.wheelAction = 0;
	if (std::isnan(s.wheelNavThreshold)) s.wheelNavThreshold = kDefaultWheelNavThreshold;
	s.wheelNavThreshold = std::clamp(s.wheelNavThreshold, 0.1f, 100.0f);
	if (s.navZoomMode < 0 || s.navZoomMode > 2) s.navZoomMode = 0;

	if (std::isnan(s.uiScale)) s.uiScale = kDefaultUiScale;
	s.uiScale = std::clamp(s.uiScale, kMinUiScale, kMaxUiScale);

	switch (s.hdrMode)
	{
	case HdrMode::Off:
	case HdrMode::Auto:
	case HdrMode::Force:
		break;
	default:
		s.hdrMode = HdrMode::Off;
		break;
	}

	switch (s.hdrTransfer)
	{
	case HdrTransfer::ScRgb:
	case HdrTransfer::Pq:
		break;
	default:
		s.hdrTransfer = HdrTransfer::ScRgb;
		break;
	}

	if (s.hdrSdrWhiteNits <= 0)
		s.hdrSdrWhiteNits = 0;
	else
		s.hdrSdrWhiteNits = std::clamp(s.hdrSdrWhiteNits,
			kMinHdrSdrWhiteNits, kMaxHdrSdrWhiteNits);

	if (s.hdrPeakNits <= 0)
		s.hdrPeakNits = 0;
	else
		s.hdrPeakNits = std::clamp(s.hdrPeakNits,
			kMinHdrPeakNits, kMaxHdrPeakNits);
}

/* Canonical serialization.  The caller is responsible for Sanitize(); Save()
 * does that on a copy before calling this. */
inline std::string Serialize(const Settings& s)
{
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "# ImageViewer settings\n";
	out << "canvas_bg = " << s.canvasBg[0] << "," << s.canvasBg[1] << "," << s.canvasBg[2] << "\n";
	out << "fade_delay_ms = " << s.fadeDelayMs << "\n";
	out << "fade_min_alpha = " << s.fadeMinAlpha << "\n";
	const char* const filterMode = FilterModeName(s.filterMode);
	out << "filter_mode = " << (filterMode ? filterMode : "none") << "\n";
	out << "pwm_bits = " << s.pwmBits << "\n";
	out << "window_width = " << s.windowWidth << "\n";
	out << "window_height = " << s.windowHeight << "\n";
	/* Written only once a position has been observed: an absent pair means
	 * "centre the window", which is not the same as placing it at (0,0). */
	if (s.windowX != kWindowPositionUnset && s.windowY != kWindowPositionUnset)
	{
		out << "window_x = " << s.windowX << "\n";
		out << "window_y = " << s.windowY << "\n";
	}
	out << "window_maximized = " << (s.windowMaximized ? "true" : "false") << "\n";
	out << "initial_zoom = " << (s.initialZoomFit ? "fit" : "100") << "\n";
	out << "chrome_visible_on_start = " << (s.chromeVisible ? "true" : "false") << "\n";
	out << "wheel_action = " << (s.wheelAction == 1 ? "next_prev" : "zoom") << "\n";
	out << "wheel_nav_threshold = " << s.wheelNavThreshold << "\n";
	out << "nav_zoom_mode = " << (s.navZoomMode == 1 ? "fit" : (s.navZoomMode == 2 ? "100" : "preserve")) << "\n";
	out << "ui_scale = " << s.uiScale << "\n";
	out << "show_readout = " << (s.showReadout ? "true" : "false") << "\n";
	out << "show_control_strip = " << (s.showControlStrip ? "true" : "false") << "\n";
	out << "remember_window_size = " << (s.rememberWindowSize ? "true" : "false") << "\n";
	const char* const hdrMode = HdrModeName(s.hdrMode);
	out << "hdr_mode = " << (hdrMode ? hdrMode : "off") << "\n";
	const char* const hdrTransfer = HdrTransferName(s.hdrTransfer);
	out << "hdr_transfer = " << (hdrTransfer ? hdrTransfer : "scrgb") << "\n";
	out << "hdr_sdr_white_nits = " << s.hdrSdrWhiteNits << "\n";
	out << "hdr_peak_nits = " << s.hdrPeakNits << "\n";
	out << "hdr_tone_map = " << (s.hdrToneMap ? "true" : "false") << "\n";
	return out.str();
}

inline std::filesystem::path ExecutableDir()
{
#if defined(_WIN32)
	char buffer[MAX_PATH] = {0};
	const DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return {};
	return std::filesystem::path(buffer).parent_path();
#else
	std::error_code ec;
	auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
	if (ec) return {};
	return p.parent_path();
#endif
}

/* %APPDATA%/ImageViewer/iview.conf, or the XDG equivalent. */
inline std::filesystem::path AppDataConfigPath()
{
#if defined(_WIN32)
	const char* appdata = std::getenv("APPDATA");
	if (appdata == nullptr || appdata[0] == '\0') return {};
	return std::filesystem::path(appdata) / "ImageViewer" / "iview.conf";
#else
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	if (xdg != nullptr && xdg[0] != '\0')
		return std::filesystem::path(xdg) / "ImageViewer" / "iview.conf";
	const char* home = std::getenv("HOME");
	if (home == nullptr || home[0] == '\0') return {};
	return std::filesystem::path(home) / ".config" / "ImageViewer" / "iview.conf";
#endif
}

/* Parses one `key = value` line.  Returns false for unknown keys or bad values;
 * the caller decides how loudly to complain. */
inline bool ApplyLine(Settings& s, const std::string& key, const std::string& value, bool& known)
{
	known = true;
	if (key == "canvas_bg")
	{
		int rgb[3];
		if (!ParseRgb(value, rgb)) return false;
		s.canvasBg[0] = rgb[0]; s.canvasBg[1] = rgb[1]; s.canvasBg[2] = rgb[2];
		return true;
	}
	if (key == "fade_delay_ms")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.fadeDelayMs = v;
		return true;
	}
	if (key == "fade_min_alpha")
	{
		float v = 0.0f;
		if (!ParseFloat(value, v)) return false;
		s.fadeMinAlpha = v;
		return true;
	}
	if (key == "filter_mode")
	{
		uint32_t mode = kFilterModeNone;
		if (!ParseFilterMode(value, mode)) return false;
		s.filterMode = mode;
		return true;
	}
	if (key == "pwm_bits")
	{
		uint32_t v = 0;
		if (!ParseU32(value, v)) return false;
		s.pwmBits = v;
		return true;
	}
	if (key == "window_width")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.windowWidth = v;
		return true;
	}
	if (key == "window_height")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.windowHeight = v;
		return true;
	}
	if (key == "window_x")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.windowX = v;
		return true;
	}
	if (key == "window_y")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.windowY = v;
		return true;
	}
	if (key == "window_maximized")
	{
		bool v = false;
		if (!ParseBool(value, v)) return false;
		s.windowMaximized = v;
		return true;
	}
	if (key == "initial_zoom")
	{
		if (value == "fit")     { s.initialZoomFit = true;  return true; }
		if (value == "100")     { s.initialZoomFit = false; return true; }
		return false;
	}
	if (key == "wheel_action")
	{
		if (value == "zoom")      { s.wheelAction = 0; return true; }
		if (value == "next_prev") { s.wheelAction = 1; return true; }
		return false;
	}
	if (key == "wheel_nav_threshold")
	{
		float v = 0.0f;
		if (!ParseFloat(value, v)) return false;
		s.wheelNavThreshold = v;
		return true;
	}
	if (key == "nav_zoom_mode")
	{
		if (value == "preserve") { s.navZoomMode = 0; return true; }
		if (value == "fit")      { s.navZoomMode = 1; return true; }
		if (value == "100")      { s.navZoomMode = 2; return true; }
		return false;
	}
	if (key == "chrome_visible_on_start")
	{
		bool v = true;
		if (!ParseBool(value, v)) return false;
		s.chromeVisible = v;
		return true;
	}
	if (key == "ui_scale")
	{
		float v = 0.0f;
		if (!ParseFloat(value, v)) return false;
		s.uiScale = v;
		return true;
	}
	if (key == "show_readout")
	{
		bool v = true;
		if (!ParseBool(value, v)) return false;
		s.showReadout = v;
		return true;
	}
	if (key == "show_control_strip")
	{
		bool v = true;
		if (!ParseBool(value, v)) return false;
		s.showControlStrip = v;
		return true;
	}
	if (key == "remember_window_size")
	{
		bool v = true;
		if (!ParseBool(value, v)) return false;
		s.rememberWindowSize = v;
		return true;
	}
	if (key == "hdr_mode")
	{
		HdrMode v = HdrMode::Off;
		if (!ParseHdrMode(value, v)) return false;
		s.hdrMode = v;
		return true;
	}
	if (key == "hdr_transfer")
	{
		HdrTransfer v = HdrTransfer::ScRgb;
		if (!ParseHdrTransfer(value, v)) return false;
		s.hdrTransfer = v;
		return true;
	}
	if (key == "hdr_sdr_white_nits")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.hdrSdrWhiteNits = v;
		return true;
	}
	if (key == "hdr_peak_nits")
	{
		int v = 0;
		if (!ParseInt(value, v)) return false;
		s.hdrPeakNits = v;
		return true;
	}
	if (key == "hdr_tone_map")
	{
		bool v = true;
		if (!ParseBool(value, v)) return false;
		s.hdrToneMap = v;
		return true;
	}

	known = false;
	return false;
}

/* Reads `path` into `s`.  Returns true when the file was opened. */
inline bool LoadFile(Settings& s, const std::filesystem::path& path, bool verbose)
{
	std::ifstream in(path);
	if (!in.is_open()) return false;

	std::string line;
	int lineNo = 0;
	while (std::getline(in, line))
	{
		++lineNo;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const auto hash = line.find('#');
		if (hash != std::string::npos) line.erase(hash);
		line = Trim(line);
		if (line.empty()) continue;

		const auto eq = line.find('=');
		if (eq == std::string::npos)
		{
			if (verbose)
				std::fprintf(stderr, "iview: %s:%d: not a key=value line, ignored\n",
				             path.string().c_str(), lineNo);
			continue;
		}

		const std::string key   = Trim(line.substr(0, eq));
		const std::string value = Trim(line.substr(eq + 1));
		bool known = false;
		if (!ApplyLine(s, key, value, known))
		{
			std::fprintf(stderr, "iview: %s:%d: %s '%s', using the builtin default\n",
			             path.string().c_str(), lineNo,
			             known ? "invalid value for" : "unknown key", key.c_str());
		}
	}
	return true;
}

/* Resolution order per the header comment.  Always fills s.savePath and always
 * applies Sanitize() to the final result. */
inline Settings Load(const std::string& cliPath, bool verbose)
{
	Settings s;                    /* builtin defaults */
	const std::filesystem::path appdata = AppDataConfigPath();
	s.savePath = appdata.empty() ? std::string{} : appdata.string();

	bool loaded = false;
	if (!cliPath.empty())
	{
		s.savePath = cliPath;
		if (LoadFile(s, cliPath, verbose))
		{
			s.loadPath = cliPath;
			loaded = true;
		}
		else
		{
			std::fprintf(stderr, "iview: --config=%s not readable, falling back\n", cliPath.c_str());
		}
	}

	if (!loaded && !appdata.empty() && LoadFile(s, appdata, verbose))
	{
		s.loadPath = appdata.string();
		loaded = true;
	}

	if (!loaded)
	{
		const std::filesystem::path besideExe = ExecutableDir() / "iview.conf";
		if (!besideExe.empty() && LoadFile(s, besideExe, verbose))
		{
			s.loadPath = besideExe.string();
			if (s.savePath.empty()) s.savePath = s.loadPath;
			loaded = true;
		}
	}

	Sanitize(s);
	return s;
}

inline bool ReplaceFile(const std::filesystem::path& from,
                        const std::filesystem::path& to)
{
#if defined(_WIN32)
	return MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
	                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

inline void RemoveTempFile(const std::filesystem::path& path) noexcept
{
	std::error_code ec;
	if (std::filesystem::is_regular_file(path, ec))
		std::filesystem::remove(path, ec);
}

/* Atomic and idempotent.  Values are sanitized before serialization, so Save()
 * never writes a value that Load()/Sanitize() would clamp back.  Returns false
 * on any I/O failure and leaves the previous file untouched. */
inline bool Save(const Settings& s)
{
	Settings clean = s;
	Sanitize(clean);
	if (clean.savePath.empty()) return false;

	const std::filesystem::path target(clean.savePath);
	const std::string contents = Serialize(clean);

	/* Idempotent: do not rewrite a file whose bytes already match.  This keeps
	 * save-on-exit from touching the config when nothing changed. */
	{
		std::ifstream existing(target, std::ios::binary);
		if (existing.is_open())
		{
			std::ostringstream current;
			current << existing.rdbuf();
			if (current.str() == contents) return true;
		}
	}

	std::error_code ec;
	const std::filesystem::path dir = target.parent_path();
	if (!dir.empty())
	{
		std::filesystem::create_directories(dir, ec);
		if (ec)
		{
			std::fprintf(stderr, "iview: cannot create directory for %s: %s\n",
			             target.string().c_str(), ec.message().c_str());
			return false;
		}
	}

	std::filesystem::path temp = target;
	temp += ".tmp";

	std::ofstream out(temp, std::ios::binary | std::ios::trunc);
	if (!out.is_open())
	{
		std::fprintf(stderr, "iview: cannot write %s\n", temp.string().c_str());
		return false;
	}

	out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	out.flush();
	if (!out)
	{
		out.close();
		RemoveTempFile(temp);
		std::fprintf(stderr, "iview: cannot write %s\n", temp.string().c_str());
		return false;
	}

	out.close();
	if (!out)
	{
		RemoveTempFile(temp);
		std::fprintf(stderr, "iview: cannot write %s\n", temp.string().c_str());
		return false;
	}

	/* No portable fsync: std::ofstream has no file descriptor.  The file is
	 * flushed and closed before the atomic replace; add a platform flush if
	 * power-loss durability becomes a requirement. */
	if (!ReplaceFile(temp, target))
	{
		RemoveTempFile(temp);
		std::fprintf(stderr, "iview: cannot replace %s\n", target.string().c_str());
		return false;
	}
	return true;
}

} // namespace Config
