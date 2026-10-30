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
 * %APPDATA% location (the directory is created on demand).
 *
 * Unknown keys warn to stderr and are ignored; an unparseable value falls back
 * to the builtin default with one warning.  A broken config never fails start-up.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace Config {

struct Settings
{
	int         canvasBg[3]   = { 128, 128, 128 };
	int         fadeDelayMs   = 1500;
	float       fadeMinAlpha  = 0.15f;
	uint32_t    filterMode    = 0;      /* IWFilterMode: 0 = none, 3 = pwm  */
	uint32_t    pwmBits       = 1;
	int         windowWidth   = 1280;
	int         windowHeight  = 720;
	bool        initialZoomFit = true;  /* false = 100 %                    */
	bool        chromeVisible = true;
	int         wheelAction   = 0;      /* 0 = zoom, 1 = prev/next           */
	float       wheelNavThreshold = 1.0f;
	int         navZoomMode   = 0;      /* 0 = preserve, 1 = fit, 2 = 100 %  */

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
	const unsigned long v = std::strtoul(text.c_str(), &end, 10);
	if (end == nullptr || *end != '\0') return false;
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
		if (!ParseInt(value, v) || v < 0 || v > 60000) return false;
		s.fadeDelayMs = v;
		return true;
	}
	if (key == "fade_min_alpha")
	{
		float v = 0.0f;
		if (!ParseFloat(value, v) || v < 0.0f || v > 1.0f) return false;
		s.fadeMinAlpha = v;
		return true;
	}
	if (key == "filter_mode")
	{
		if (value == "none")    { s.filterMode = 0u; return true; }
		if (value == "pwm")     { s.filterMode = 3u; return true; }
		return false;
	}
	if (key == "pwm_bits")
	{
		uint32_t v = 0;
		if (!ParseU32(value, v) || v < 1u || v > 8u) return false;
		s.pwmBits = v;
		return true;
	}
	if (key == "window_width")
	{
		int v = 0;
		if (!ParseInt(value, v) || v < 64 || v > 16384) return false;
		s.windowWidth = v;
		return true;
	}
	if (key == "window_height")
	{
		int v = 0;
		if (!ParseInt(value, v) || v < 64 || v > 16384) return false;
		s.windowHeight = v;
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
		if (!ParseFloat(value, v) || v < 0.1f || v > 100.0f) return false;
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

/* Resolution order per the header comment.  Always fills s.savePath. */
inline Settings Load(const std::string& cliPath, bool verbose)
{
	Settings s;                    /* builtin defaults */
	const std::filesystem::path appdata = AppDataConfigPath();
	s.savePath = appdata.empty() ? std::string{} : appdata.string();

	if (!cliPath.empty())
	{
		s.savePath = cliPath;
		if (LoadFile(s, cliPath, verbose)) { s.loadPath = cliPath; return s; }
		std::fprintf(stderr, "iview: --config=%s not readable, falling back\n", cliPath.c_str());
	}

	if (!appdata.empty() && LoadFile(s, appdata, verbose))
	{
		s.loadPath = appdata.string();
		return s;
	}

	const std::filesystem::path besideExe = ExecutableDir() / "iview.conf";
	if (!besideExe.empty() && LoadFile(s, besideExe, verbose))
	{
		s.loadPath = besideExe.string();
		if (s.savePath.empty()) s.savePath = s.loadPath;
		return s;
	}

	return s;
}

inline bool Save(const Settings& s)
{
	if (s.savePath.empty()) return false;

	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::path(s.savePath).parent_path();
	if (!dir.empty()) std::filesystem::create_directories(dir, ec);

	std::ofstream out(s.savePath, std::ios::trunc);
	if (!out.is_open())
	{
		std::fprintf(stderr, "iview: cannot write %s\n", s.savePath.c_str());
		return false;
	}

	out << "# ImageViewer settings\n";
	out << "canvas_bg = " << s.canvasBg[0] << "," << s.canvasBg[1] << "," << s.canvasBg[2] << "\n";
	out << "fade_delay_ms = " << s.fadeDelayMs << "\n";
	out << "fade_min_alpha = " << s.fadeMinAlpha << "\n";
	out << "filter_mode = " << (s.filterMode == 3u ? "pwm" : "none") << "\n";
	out << "pwm_bits = " << s.pwmBits << "\n";
	out << "window_width = " << s.windowWidth << "\n";
	out << "window_height = " << s.windowHeight << "\n";
	out << "initial_zoom = " << (s.initialZoomFit ? "fit" : "100") << "\n";
	out << "chrome_visible_on_start = " << (s.chromeVisible ? "true" : "false") << "\n";
	out << "wheel_action = " << (s.wheelAction == 1 ? "next_prev" : "zoom") << "\n";
	out << "wheel_nav_threshold = " << s.wheelNavThreshold << "\n";
	out << "nav_zoom_mode = " << (s.navZoomMode == 1 ? "fit" : (s.navZoomMode == 2 ? "100" : "preserve")) << "\n";
	return out.good();
}

} // namespace Config
