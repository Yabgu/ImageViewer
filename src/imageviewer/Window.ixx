module;

#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <utility>
#include <future>
#include <mutex>
#include <optional>
#include <exception>
#include <string>
#include <format>
#include <filesystem>
#include <system_error>

/* One GL loader for the whole process: glad.  ImGui's OpenGL3 backend is built
 * with IMGUI_IMPL_OPENGL_LOADER_CUSTOM and force-includes glad/glad.h, so it
 * uses the same function pointers as this module. */
#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#include <share.h>
/* glfwGetWin32Window: the window handle is needed for SetWindowPlacement, see
 * ShowInitialWindow(). */
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "FilterPluginDef.h"
#include "FilterCore.hpp"
#include "FilterJob.hpp"
#include "Config.hpp"
#include "ZoomScale.hpp"
#include "resources/ResourceIds.h"

export module UserInterface;

import Image;
import HotkeysHandler;
import FilterPlugin;

/* ── Startup trace ────────────────────────────────────────────────────────────
 *
 * Milliseconds since the first trace call in the process.  Enabled when
 * IVIEW_TRACE is set to something other than "0".  IMAGEVIEWER_VK_TRACE is
 * still honored for existing probe scripts.  If IVIEW_TRACE_FILE is also set,
 * lines are appended to that file as well as stderr; this is how GUI-subsystem
 * runs (which have no usable stderr) are captured.
 *
 * Exported because main_generic.hpp marks "async decode launched". */
static const std::chrono::steady_clock::time_point& IviewTraceStart() noexcept
{
	static const std::chrono::steady_clock::time_point t0 =
		std::chrono::steady_clock::now();
	return t0;
}

static std::mutex& IviewTraceMutex() noexcept
{
	static std::mutex mutex;
	return mutex;
}

static FILE* IviewTraceFile() noexcept
{
	static FILE* file = [] {
		const char* path = std::getenv("IVIEW_TRACE_FILE");
		if (path == nullptr || path[0] == '\0') return static_cast<FILE*>(nullptr);
#if defined(_WIN32)
		/* _SH_DENYNO lets a diagnostics harness tail the file while the app runs. */
		return _fsopen(path, "ab", _SH_DENYNO);
#else
		return std::fopen(path, "ab");
#endif
	}();
	return file;
}

export bool IviewTraceEnabled() noexcept
{
	static const bool enabled = [] {
		const char* v = std::getenv("IVIEW_TRACE");
		if (v == nullptr || v[0] == '\0' || std::strcmp(v, "0") == 0)
			v = std::getenv("IMAGEVIEWER_VK_TRACE");
		return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
	}();
	return enabled;
}

export void IviewTrace(const char* what) noexcept
{
	if (!IviewTraceEnabled()) return;
	try
	{
		const auto t0 = IviewTraceStart();
		const double ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
		std::lock_guard<std::mutex> lock(IviewTraceMutex());
		std::fprintf(stderr, "IVIEW_TRACE +%8.2f ms  %s\n", ms, what);
		std::fflush(stderr);
		if (FILE* file = IviewTraceFile())
		{
			std::fprintf(file, "IVIEW_TRACE +%8.2f ms  %s\n", ms, what);
			std::fflush(file);
		}
	}
	catch (...)
	{
		/* Diagnostics must never change behavior. */
	}
}

/* Canvas background: mid-gray #808080 — the same glClearColor the OpenGL branch
 * used, so an image that does not fill the window sits on the same backdrop. */
static constexpr float kCanvasBg[3] = {
	128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f
};

/* All Dear ImGui state lives here (pimpl; no ImGui type is exported). */
struct WindowImGuiState
{
	bool  initialized       = false;
	bool  chromeEnabled     = true;   /* Tab toggles this */
	bool  frameClockStarted = false;
	float alpha             = 0.0f;
	std::string readout;              /* "1920x1080 . 3.2 MB", filled once on upload */
	std::chrono::steady_clock::time_point lastMouseActivity{};
	ImGuiStyle baseStyle{};           /* unscaled style, captured before ScaleAllSizes */
};

/* Two pre-quantised 8-bit frames as the filter plugin produced them.  Built on a
 * worker thread (no GL involved), uploaded on the main thread. */
struct FilterFrameSet
{
	std::vector<uint8_t> phaseA;
	std::vector<uint8_t> phaseB;
	int width  = 0;
	int height = 0;
};

export class Window
{
protected:
	static Window* From(GLFWwindow* window) noexcept
	{
		return reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
	}

	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = From(window);
		if (win != nullptr)
		{
			win->width  = width;
			win->height = height;
		}
	}

	static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = From(window);
		if (win != nullptr)
		{
			/* A resize can arrive before the GL context is current (applying the
			 * saved placement resizes the hidden window during construction);
			 * glViewport would then call a null glad entry point. */
			if (glfwGetCurrentContext() == window) glViewport(0, 0, width, height);
			/* The viewport follows the framebuffer, which is what the quad's
			 * projection is computed against. */
			win->width  = width;
			win->height = height;
		}
	}

	static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
	{
		Window* win = From(window);
		if (win == nullptr) return;
		/* Let the ImGui chrome keep the wheel when the cursor is over it. */
		if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) return;

		if (win->settings_.wheelAction == 1)
		{
			/* Prev/next: accumulate notches, one navigation per threshold.  The
			 * coalescing decoder collapses a fast spin to one decode. */
			win->wheel_accum_ += static_cast<float>(yoffset);
			const float threshold = std::max(0.1f, win->settings_.wheelNavThreshold);
			while (std::fabs(win->wheel_accum_) >= threshold)
			{
				if (win->wheel_accum_ > 0.0f) { win->wheel_accum_ -= threshold; win->NavigateRelative(-1); }  /* up = prev */
				else                          { win->wheel_accum_ += threshold; win->NavigateRelative(+1); }
			}
			return;
		}

		win->applyScroll(xoffset, yoffset);
	}

	/* GLFW file drop.  ImGui's GLFW backend does not install a drop callback, so
	 * this one survives ImGui_ImplGlfw_InitForOpenGL(..., true) untouched. */
	static void drop_callback(GLFWwindow* window, int count, const char** paths)
	{
		Window* win = From(window);
		if (win != nullptr) win->HandleDrop(count, paths);
	}

	void applyScroll(double /*xoffset*/, double yoffset)
	{
		double mouseX, mouseY;
		glfwGetCursorPos(glfwWindow, &mouseX, &mouseY);

		/* One wheel notch is one zoom-scale step; precise wheels send fractional
		 * offsets and get a proportionally smaller step. */
		setZoom(ZoomScale::Step(getZoom(), yoffset), mouseX, mouseY);
	}

private:
	GLFWwindow* glfwWindow = nullptr;
	int width;
	int height;
	/* Linear magnification; 1.0 = 100 %.  Every user-facing step is geometric
	 * (see ZoomScale.hpp), and a double keeps repeated steps from quantising. */
	double zoom_;
	/* Last client-area geometry observed while the window was in its normal state
	 * (neither maximized nor iconified).  GLFW exposes no restore rectangle, so
	 * this sample is what gets persisted: a maximized window comes back maximized
	 * *and* un-maximizes to where the user left it. */
	int  normalX_ = 0;
	int  normalY_ = 0;
	int  normalW_ = 0;
	int  normalH_ = 0;
	bool normalGeometryKnown_ = false;
	int imageWidth_  = 0;
	int imageHeight_ = 0;
	FilterPlugin filterPlugin_;
	double panX = 0.0;
	double panY = 0.0;
	bool isPanning = false;
	double lastMouseX = 0.0, lastMouseY = 0.0;

	/* Dear ImGui private state (see WindowImGuiState at module scope). */
	std::unique_ptr<WindowImGuiState> imgui_;
	/* Owned copy of the filter options from the command line. */
	IWFilterOptions filterOptions_{ IW_FILTER_OPTIONS_VERSION, 0u, nullptr };
	IWFilterArg filterArgStorage_[5]{};

	/* Source pixels of the file currently shown, retained so a filter change can
	 * be recomputed without decoding again.  This costs the decoded image for
	 * the lifetime of the file — ~75 MB for a 4K 16-bit image, ~38 MB for 4K
	 * RGB8 — which is the accepted price of live filter switching.
	 *
	 * shared_ptr<const Image>: an in-flight filter worker captures its own
	 * reference, so navigation may replace this slot without freeing the image
	 * out from under the worker.  const because ComputeFilteredFrames() only
	 * reads the source. */
	std::shared_ptr<const Image> source_image_;

	/* Progressive initial load workers.  Main() consumes the shared state, but
	 * the futures are kept here so the destructor can drain them before GL
	 * teardown.  The preview future is absent for loaders without a preview. */
	std::future<void>           initial_preview_worker_;
	std::future<void>           initial_worker_;

	/* Async filter re-run.  Only one compute may be in flight: the plugin is a
	 * single DLL handle with no documented reentrancy, and its result is
	 * uploaded on the main thread. */
	std::future<FilterFrameSet> filter_future_;
	bool                        filter_running_ = false;
	uint32_t                    appliedMode_    = 0;   /* mode in the textures */
	uint64_t                    filterGeneration_ = 0; /* file the compute belongs to */
	std::chrono::steady_clock::time_point filterStartTime_{};
	bool                        filter_failed_  = false;
	std::string                 filter_error_;

	/* Settings: builtin defaults < config file < CLI (merged in main_generic). */
	Config::Settings settings_;
	int      fadeDelayMs_   = 1500;
	float    fadeMinAlpha_  = 0.15f;
	float    canvasBg_[3]   = { 128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f };
	uint32_t filterMode_    = 0;   /* current selection (radio state)        */
	uint32_t pwmBits_       = 1;   /* temporal bits for PWM                  */
	uint32_t spatialBits_   = 0;   /* preserved from the CLI, unused by PWM  */
	uint64_t seed_          = 0;   /* preserved from the CLI                 */
	bool     settings_open_ = false;
	bool     settings_dirty_ = false;
	bool     settings_focus_requested_ = false;
	bool     settings_status_ok_ = true;
	std::string settings_status_;

	enum class SettingsTab { Interface, Viewing, Filter, Advanced };
	SettingsTab settings_tab_request_ = SettingsTab::Interface;
	SettingsTab active_settings_tab_ = SettingsTab::Interface;
	bool settings_tab_request_pending_ = false;

	/* Set when an image was uploaded in mode=none and the requested mode still
	 * has to be computed; consumed once the window is visible. */
	bool     pendingDeferredFilter_ = false;

	/* ── Folder navigation ─────────────────────────────────────────────────── */
	std::vector<std::filesystem::path> folder_images_;
	size_t   folder_index_     = 0;

	struct DecodeResult
	{
		size_t index;
		Image  image;
	};
	std::future<DecodeResult> nav_future_;
	bool     nav_in_flight_    = false;
	size_t   nav_target_index_ = 0;   /* what the user wants         */
	size_t   nav_shown_index_  = 0;   /* what is on screen           */

	std::filesystem::path current_path_;   /* file on screen (or loading) */
	uint64_t file_generation_ = 0;         /* bumped per upload; stale-result guard */
	std::string readout_dims_;             /* "WxH . N MB", built once per file     */
	std::string last_error_;               /* nav decode failure for the readout    */
	float    wheel_accum_     = 0.0f;

	enum class ZoomMode { Fit, Pct100, Manual };
	ZoomMode zoom_mode_ = ZoomMode::Fit;
	/* One-shot UI rect logs, used by the automated strip/panel check. */
	bool     stripRectsLogged_  = false;
	int      chromeFrames_      = 0;

	/* ── GL resources ──────────────────────────────────────────────────────── */

	/* GL_MAX_TEXTURE_SIZE is a hard limit, and panoramas and stitched
	 * screenshots routinely exceed it (17277 x 8176 is one PNG in the wild, and
	 * 16384 is a typical limit).  The raster is therefore cut into a grid of
	 * tiles that each fit, and every tile carries a one-texel gutter on all four
	 * sides holding the neighbouring source pixels:
	 *
	 *     +-+-------+--+     the gutter makes GL_LINEAR interpolate across a tile
	 *     | |       |  |     boundary with the real neighbour instead of clamping
	 *     +-+-------+--+     to the tile's own edge texel.  Without it every seam
	 *     | |  tile |  |     shows as a faint line, which is the artifact that
	 *     +-+-------+--+     makes naive tiling look broken when minified.
	 *     | |       |  |
	 *     +-+-------+--+     The gutter is never drawn: each quad's texture
	 *                       coordinates start and end on the body's outer texel
	 *                       edges, so neighbouring quads meet exactly.
	 *
	 * Gutters also make GL_NEAREST exact at a fractional sub-pixel offset: a
	 * fragment that rounds across a tile boundary lands on the gutter texel,
	 * which holds precisely the pixel the adjacent tile would have shown.
	 */
	static constexpr int kTextureBorder = 1;
	/* Preferred upper bound on a single texture's extent.  GL_MAX_TEXTURE_SIZE
	 * wins whenever it is smaller; above this, splitting into a few more tiles
	 * costs a handful of draw calls but keeps every allocation at a size desktop
	 * drivers hand out reliably. */
	static constexpr int kPreferredTileExtent = 16384;

	struct TextureTile
	{
		GLuint id   = 0;
		int    texW = 0;   /* texture size in texels, gutters included */
		int    texH = 0;
		int    x0   = 0;   /* source-raster pixel rect this tile covers, */
		int    y0   = 0;   /* measured without the gutters              */
		int    w    = 0;
		int    h    = 0;
	};

	/* One frame's worth of tiles, i.e. the whole picture.  Two passes exist when
	 * the filter produced two phase frames; they are drawn alternately. */
	struct TexturePass
	{
		std::vector<TextureTile> tiles;
	};

	TexturePass passes_[2];    /* phase A / phase B (identical when single-frame) */
	int         passCount_ = 0;

	int rasterWidth_    = 0;   /* dims of the raster actually uploaded; a preview */
	int rasterHeight_   = 0;   /* raster is smaller than imageWidth_/imageHeight_  */
	int maxTextureSize_ = 0;   /* GL_MAX_TEXTURE_SIZE, read after context init    */
	int tileExtent_     = 0;   /* tile body size that fits, gutters excluded      */

	GLuint shaderProgram_  = 0;
	GLuint vao_            = 0;
	GLuint vboPositions_   = 0;
	GLuint vboTexCoords_   = 0;
	GLint  uniformProjection_ = -1;
	GLint  uniformModel_      = -1;
	GLint  uniformTexture_    = -1;
	GLint  uniformUvRect_     = -1;

	/* ── GPU dither backend ────────────────────────────────────────────────── */
	/*
	 * Instead of running the filter plugin on the CPU and uploading finished
	 * 8-bit frames, the GPU backend uploads the source at its NATIVE precision
	 * and quantises in the fragment shader.  Two consequences:
	 *
	 *  - Opening a file never waits for a dither pass.  On a 51 MP 16-bit
	 *    source the CPU pass is ~27 s serial / ~3 s on 16 cores; the shader
	 *    version costs one extra ALU chain per fragment and nothing else.
	 *  - Changing filter mode is free.  The mode is a uniform, so switching
	 *    Bayer/blue-noise/spatial-temporal no longer re-runs anything.
	 *
	 * The price is precision, and it is real: this is float arithmetic in the
	 * shader rather than the plugin's integer path, so the result is visually
	 * equivalent but NOT byte-identical to the CPU backend.  That is why the
	 * backend is a choice and why cpu stays the default.
	 */
	Config::FilterBackend filterBackend_ = Config::FilterBackend::Cpu;
	bool ditherReady_ = false;     /* shader compiled and blue-noise uploaded   */
	bool sourceIsNative_ = false;  /* current tiles hold source, not RGBA8      */
	int  nativeComponents_ = 4;    /* channels the dither shader sees           */

	GLuint ditherProgram_    = 0;
	GLuint blueNoiseTex_     = 0;  /* 64x64 R16, ranks normalised to 0..1       */
	GLint  ditherProjection_ = -1;
	GLint  ditherModel_      = -1;
	GLint  ditherUvRect_     = -1;
	GLint  ditherTileRect_   = -1;
	GLint  ditherSource_     = -1;
	GLint  ditherNoise_      = -1;
	GLint  ditherMode_       = -1;
	GLint  ditherPhase_      = -1;
	GLint  ditherComponents_ = -1;
	GLint  ditherSpace_      = -1;
	GLint  ditherSeed_       = -1;

	double getZoom() const {
		return zoom_;
	}

	int getZoomPercent() const noexcept {
		return ZoomScale::Percent(zoom_);
	}

	bool HasImage() const noexcept { return imageWidth_ > 0 && imageHeight_ > 0; }

	/* True when the ImGui chrome currently owns the mouse (hovering a widget). */
	static bool ImguiWantsMouse() noexcept {
		if (ImGui::GetCurrentContext() == nullptr) return false;
		return ImGui::GetIO().WantCaptureMouse;
	}

	void setPanning(double x, double y) {
		if (HasImage()) {
			const double zoom = getZoom();
			const double imgW = imageWidth_ * zoom;
			const double imgH = imageHeight_ * zoom;
			const double minPanX = -(imgW - width) * 0.5;
			const double maxPanX = (imgW - width) * 0.5;
			const double minPanY = -(imgH - height) * 0.5;
			const double maxPanY = (imgH - height) * 0.5;

			panX = (imgW < width)  ? 0.0 : std::max(minPanX, std::min(x, maxPanX));
			panY = (imgH < height) ? 0.0 : std::max(minPanY, std::min(y, maxPanY));
		} else {
			panX = x;
			panY = y;
		}
	}

	/* Absolute zoom change that keeps the image point under (cursorX, cursorY)
	 * in place.  setPanning() re-clamps the result, so an anchor outside the
	 * image cannot push the view off it. */
	void ZoomAround(double targetZoom, double cursorX, double cursorY)
	{
		const double currentZoom = getZoom();
		zoom_ = ZoomScale::Clamp(targetZoom);

		if (IviewTraceEnabled())
		{
			char line[64];
			std::snprintf(line, sizeof(line), "zoom: %.1f%%", zoom_ * 100.0);
			IviewTrace(line);
		}

		if (!HasImage() || !(currentZoom > 0.0)) return;

		/* Where the anchor sits in image pixels before the change. */
		const double imgX = (width  - imageWidth_  * currentZoom) * 0.5 + panX;
		const double imgY = (height - imageHeight_ * currentZoom) * 0.5 + panY;
		const double anchorImgX = (cursorX - imgX) / currentZoom;
		const double anchorImgY = (cursorY - imgY) / currentZoom;

		/* Pan that would centre the new size, before anchoring. */
		const double newImgX = (width  - imageWidth_  * zoom_) * 0.5 + panX;
		const double newImgY = (height - imageHeight_ * zoom_) * 0.5 + panY;

		setPanning(panX + cursorX - (newImgX + anchorImgX * zoom_),
		           panY + cursorY - (newImgY + anchorImgY * zoom_));
	}

	static void mouse_button_callback(GLFWwindow* window, int button, int action, int /*mods*/) {
		Window* win = From(window);
		if (button == GLFW_MOUSE_BUTTON_LEFT) {
			if (action == GLFW_PRESS) {
				if (win == nullptr || ImguiWantsMouse()) return;
				double x, y;
				glfwGetCursorPos(window, &x, &y);
				win->isPanning = true;
				win->lastMouseX = x;
				win->lastMouseY = y;
			} else if (action == GLFW_RELEASE) {
				if (win != nullptr) win->isPanning = false;
			}
		}
	}

	static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
		Window* win = From(window);
		if (win == nullptr) return;
		if (win->isPanning) {
			win->setPanning(win->panX + (xpos - win->lastMouseX),
			                win->panY + (ypos - win->lastMouseY));
			win->lastMouseX = xpos;
			win->lastMouseY = ypos;
		}
	}

	/* Client-area rectangle in screen coordinates, the same convention GLFW uses
	 * for glfwCreateWindow()/glfwSetWindowPos()/glfwGetWindowSize(). */
	struct WindowPlacement
	{
		int x      = 0;
		int y      = 0;
		int width  = 0;
		int height = 0;
	};

	/* Decoration around the content area (glfwGetWindowFrameSize).  Screens are
	 * laid out by *frame* rect, not client rect: centring a client area that is
	 * almost as tall as the work area without these extents puts the title bar
	 * above the screen. */
	struct FrameExtents
	{
		int left   = 0;
		int top    = 0;
		int right  = 0;
		int bottom = 0;

		int width()  const noexcept { return std::max(0, left) + std::max(0, right); }
		int height() const noexcept { return std::max(0, top) + std::max(0, bottom); }
	};

	struct ScreenRect
	{
		int x      = 0;
		int y      = 0;
		int width  = 0;
		int height = 0;
	};

	static ScreenRect FrameRectFor(const WindowPlacement& p, const FrameExtents& frame) noexcept
	{
		return ScreenRect{ p.x - frame.left,
		                   p.y - frame.top,
		                   p.width  + frame.width(),
		                   p.height + frame.height() };
	}

	/* True when the frame's title bar is reachable on some monitor and at least
	 * half of the window would be visible.  A saved position may name a monitor
	 * that is no longer attached, a retired layout, or coordinates outside every
	 * screen; centring is a better answer than restoring a window the user
	 * cannot grab, see or move. */
	static bool PlacementIsReachable(const WindowPlacement& p, const FrameExtents& frame) noexcept
	{
		if (p.width <= 0 || p.height <= 0) return false;
		int count = 0;
		GLFWmonitor** const monitors = glfwGetMonitors(&count);
		if (monitors == nullptr || count == 0) return false;

		const ScreenRect f = FrameRectFor(p, frame);
		if (f.width <= 0 || f.height <= 0) return false;
		const long long area = static_cast<long long>(f.width) * f.height;
		constexpr int kTitleBarMinHeight = 16;

		for (int i = 0; i < count; ++i)
		{
			int wx = 0, wy = 0, ww = 0, wh = 0;
			glfwGetMonitorWorkarea(monitors[i], &wx, &wy, &ww, &wh);
			if (ww <= 0 || wh <= 0) continue;

			const bool titleReachable = f.y >= wy &&
			                            f.y + kTitleBarMinHeight <= wy + wh &&
			                            f.x + f.width > wx && f.x < wx + ww;
			if (!titleReachable) continue;

			const long long x0 = std::max<long long>(f.x, wx);
			const long long y0 = std::max<long long>(f.y, wy);
			const long long x1 = std::min<long long>(static_cast<long long>(f.x) + f.width,
			                                        static_cast<long long>(wx) + ww);
			const long long y1 = std::min<long long>(static_cast<long long>(f.y) + f.height,
			                                        static_cast<long long>(wy) + wh);
			if (x1 <= x0 || y1 <= y0) continue;
			if ((x1 - x0) * (y1 - y0) * 2 >= area) return true;
		}
		return false;
	}

	/* A stored size can outlive the display it was measured on.  Shrink the
	 * client area so the whole frame still fits the largest work area. */
	static void ClampSizeToDisplays(int& width, int& height, const FrameExtents& frame) noexcept
	{
		int count = 0;
		GLFWmonitor** const monitors = glfwGetMonitors(&count);
		if (monitors == nullptr || count == 0) return;

		int maxWidth  = 0;
		int maxHeight = 0;
		for (int i = 0; i < count; ++i)
		{
			int wx = 0, wy = 0, ww = 0, wh = 0;
			glfwGetMonitorWorkarea(monitors[i], &wx, &wy, &ww, &wh);
			maxWidth  = std::max(maxWidth, ww);
			maxHeight = std::max(maxHeight, wh);
		}
		if (maxWidth  > 0) width  = std::min(width,  std::max(64, maxWidth  - frame.width()));
		if (maxHeight > 0) height = std::min(height, std::max(64, maxHeight - frame.height()));
	}

	static WindowPlacement CentreOnPrimary(int width, int height, const FrameExtents& frame) noexcept
	{
		WindowPlacement p{0, 0, width, height};
		GLFWmonitor* const primary = glfwGetPrimaryMonitor();
		int wx = 0, wy = 0, ww = 0, wh = 0;
		if (primary != nullptr) glfwGetMonitorWorkarea(primary, &wx, &wy, &ww, &wh);
		if (ww > 0 && wh > 0)
		{
			const int frameX = wx + (ww - (width + frame.width())) / 2;
			int       frameY = wy + (wh - (height + frame.height())) / 2;
			frameY = std::max(frameY, wy);   /* never hide the title bar */
			p.x = frameX + frame.left;
			p.y = frameY + frame.top;
		}
		return p;
	}

	static GLFWwindow* InitializeWindow(int width, int height, Window* containerWindow)
	{
		/* GL 3.3 core: everything this renderer uses (VAO, GLSL 330, layout
		 * qualifiers) is core there, and the quad shader is written for it. */
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		/* Ask for an error-free context; GLFW only sets WGL_ARB_create_context_no_error
		 * when the driver exposes it and stays silent otherwise. */
		glfwWindowHint(GLFW_CONTEXT_NO_ERROR, GLFW_TRUE);

		if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND))
			glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

		GLFWwindow* const glfwWindow = glfwCreateWindow(width, height, "ImageViewer", NULL, NULL);
		if (glfwWindow == nullptr)
			throw std::runtime_error("Failed to create window");

		glfwSetWindowUserPointer(glfwWindow, (void*)containerWindow);

#ifdef _WIN32
		/* GLFW registers its Win32 window class with only the large icon, taken
		 * from the GLFW_ICON resource in resources/iview.rc.  Without an explicit
		 * small icon the title bar gets that 32 px bitmap downscaled, so load the
		 * 16 px entry from the same icon group instead.  LR_SHARED keeps the
		 * handle valid for the window's lifetime and needs no DestroyIcon. */
		if (HWND const hwnd = glfwGetWin32Window(glfwWindow))
		{
			HICON const smallIcon = static_cast<HICON>(LoadImageW(
				GetModuleHandleW(nullptr),
				MAKEINTRESOURCEW(IVIEW_ICON_ID),
				IMAGE_ICON,
				GetSystemMetrics(SM_CXSMICON),
				GetSystemMetrics(SM_CYSMICON),
				LR_SHARED));
			if (smallIcon != nullptr)
				SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
					reinterpret_cast<LPARAM>(smallIcon));
		}
#endif

		glfwSetWindowSizeCallback(glfwWindow, window_size_callback);
		glfwSetFramebufferSizeCallback(glfwWindow, framebuffer_size_callback);
		glfwSetScrollCallback(glfwWindow, scroll_callback);
		glfwSetMouseButtonCallback(glfwWindow, mouse_button_callback);
		glfwSetCursorPosCallback(glfwWindow, cursor_position_callback);
		glfwSetKeyCallback(glfwWindow, key_callback);
		glfwSetDropCallback(glfwWindow, drop_callback);

		return glfwWindow;
	}

public:
	inline static void Initialize()
	{
		if (!glfwInit())
		{
			[[unlikely]] throw std::runtime_error("glfwInit failed");
		}
	}

	static void Deinitialize()
	{
		IviewTrace("glfwTerminate start");
		glfwTerminate();
		IviewTrace("glfwTerminate done");
	}

private:
	HotkeysHandler hotkeysHandler;

public:
	Window(int width, int height, const Config::Settings& settings)
		: glfwWindow(InitializeWindow(width, height, this)),
		  width(width),
		  height(height),
		  zoom_(1.0),
		  hotkeysHandler() {
		/* Diagnostics must interleave truthfully with the driver's own stderr. */
		std::setvbuf(stderr, nullptr, _IONBF, 0);
		IviewTrace("window created");

		settings_     = settings;
		fadeDelayMs_  = settings.fadeDelayMs;
		fadeMinAlpha_ = settings.fadeMinAlpha;
		canvasBg_[0]  = settings.canvasBg[0] / 255.0f;
		canvasBg_[1]  = settings.canvasBg[1] / 255.0f;
		canvasBg_[2]  = settings.canvasBg[2] / 255.0f;
		filterMode_   = settings.filterMode;
		filterBackend_ = settings.filterBackend;
		pwmBits_      = settings.pwmBits;
		{
			char line[192];
			std::snprintf(line, sizeof(line),
			              "settings: filter_mode=%u pwm_bits=%u window=%dx%d fade=%dms "
			              "min_alpha=%.2f bg=%d,%d,%d chrome=%d config=%s",
			              filterMode_, pwmBits_, settings.windowWidth, settings.windowHeight,
			              settings.fadeDelayMs, settings.fadeMinAlpha,
			              settings.canvasBg[0], settings.canvasBg[1], settings.canvasBg[2],
			              (int)settings.chromeVisible,
			              settings.loadPath.empty() ? "(builtin)" : settings.loadPath.c_str());
			IviewTrace(line);
		}

		glfwMakeContextCurrent(glfwWindow);
		/* Vertical sync is the loop's only pacer: glfwSwapBuffers blocks until the
		 * next refresh, so the two phases alternate at the display rate. */
		glfwSwapInterval(1);

		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			[[unlikely]] throw std::runtime_error("gladLoadGLLoader failed");
		}
		IviewTrace("GL context created");

		/* Placement is applied only now: resizing fires WM_SIZE and
		 * framebuffer_size_callback calls glViewport, which needs a current
		 * context.  The window is still hidden, so nothing of this is visible. */
		const WindowPlacement placement = ApplyInitialPlacement(settings_);
		normalX_ = placement.x;
		normalY_ = placement.y;
		normalW_ = placement.width;
		normalH_ = placement.height;
		normalGeometryKnown_ = true;
		{
			char line[192];
			std::snprintf(line, sizeof(line),
			              "window placement: %dx%d at %d,%d maximized=%d",
			              placement.width, placement.height, placement.x, placement.y,
			              glfwGetWindowAttrib(glfwWindow, GLFW_MAXIMIZED) == GLFW_TRUE ? 1 : 0);
			IviewTrace(line);
		}

		glEnable(GL_TEXTURE_2D);

		ApplyTextureLimits();
		CreateQuadResources();
		/* Built unconditionally: an unusable shader is detected here and simply
		 * leaves the CPU backend as the only option, so choosing "gpu" in the
		 * settings file on a machine that cannot run it degrades instead of
		 * failing to start. */
		CreateDitherResources();
		InitImGui();
		imgui_->chromeEnabled = settings.chromeVisible;
		IviewTrace("ImGui initialized (opengl3 backend)");

		hotkeysHandler.setZoomCallback([this](int zoomDelta) { adjustZoom(zoomDelta); });
		hotkeysHandler.setPanCallback([this](int dx, int dy) { adjustPan(dx, dy); });
		hotkeysHandler.setSetZoomCallback([this](double zoomLevel) { setZoom(zoomLevel); });
		hotkeysHandler.setFitToWindowCallback([this]() { fitToWindow(); });

		/* Try to load the default filter plugin (non-fatal if absent). */
		try {
			filterPlugin_.Load(FilterPlugin::DefaultPluginPath());
		} catch (const std::exception& ex) {
			std::cerr << "Warning: filter plugin not loaded: " << ex.what() << '\n';
		}
	}

	void SetSettingsOpen(bool open)
	{
		if (open) OpenSettings(SettingsTab::Interface, true);
		else settings_open_ = false;
	}

	~Window()
	{
		if (glfwWindow == nullptr) return;

		IviewTrace("window destroy start");
		bool drain_ok = true;
		std::string drain_failure;

		/* Drain workers before GL/GLFW teardown.  The filter worker owns a
		 * shared_ptr to its source image, but it still calls into the plugin and
		 * into this Window; both must outlive the worker.  The GL context does
		 * not need to outlive the workers today, but ordering the wait first
		 * makes that invariant explicit and avoids a future GL-using job racing
		 * glfwDestroyWindow(). */
		if (initial_preview_worker_.valid())
		{
			IviewTrace("draining initial preview future");
			try { initial_preview_worker_.wait(); }
			catch (...)
			{
				IviewTrace("initial preview future drain caught exception");
			}
		}
		if (initial_worker_.valid())
		{
			IviewTrace("draining initial progressive future");
			try { initial_worker_.wait(); }
			catch (...)
			{
				IviewTrace("initial progressive future drain caught exception");
			}
		}
		if (filter_future_.valid())
		{
			IviewTrace("draining filter future");
			try { filter_future_.wait(); (void)filter_future_.get(); }
			catch (const std::exception& ex)
			{
				drain_ok = false;
				drain_failure = std::string("filter future drain: ") + ex.what();
				IviewTrace("filter future drain caught exception");
			}
			catch (...)
			{
				drain_ok = false;
				drain_failure = "filter future drain: unknown exception";
				IviewTrace("filter future drain caught exception");
			}
			filter_running_ = false;
		}
		if (nav_future_.valid())
		{
			IviewTrace("draining nav future");
			try { nav_future_.wait(); (void)nav_future_.get(); }
			catch (const std::exception& ex)
			{
				if (drain_ok)
				{
					drain_ok = false;
					drain_failure = std::string("nav future drain: ") + ex.what();
				}
				IviewTrace("nav future drain caught exception");
			}
			catch (...)
			{
				if (drain_ok)
				{
					drain_ok = false;
					drain_failure = "nav future drain: unknown exception";
				}
				IviewTrace("nav future drain caught exception");
			}
			nav_in_flight_ = false;
		}
		if (!drain_ok)
			IviewTrace(("future drain failed: " + drain_failure).c_str());

		ShutdownImGui();
		IviewTrace("ImGui shutdown done");

		DeletePasses();
		if (blueNoiseTex_ != 0) { glDeleteTextures(1, &blueNoiseTex_); blueNoiseTex_ = 0; }
		if (ditherProgram_ != 0) { glDeleteProgram(ditherProgram_); ditherProgram_ = 0; }
		if (vboTexCoords_ != 0) glDeleteBuffers(1, &vboTexCoords_);
		if (vboPositions_ != 0) glDeleteBuffers(1, &vboPositions_);
		if (vao_ != 0)          glDeleteVertexArrays(1, &vao_);
		if (shaderProgram_ != 0) glDeleteProgram(shaderProgram_);
		IviewTrace("GL resources deleted");

		glfwDestroyWindow(glfwWindow);
		glfwWindow = nullptr;
		IviewTrace("GLFW window destroyed");
	}

private:
	/* ── Quad / shader ─────────────────────────────────────────────────────── */

	static GLuint CompileShader(GLenum type, const char* source)
	{
		const GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, NULL);
		glCompileShader(shader);

		GLint success = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
		if (!success)
		{
			char log[1024] = {0};
			glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
			glDeleteShader(shader);
			throw std::runtime_error(std::string("Shader compilation failed: ") + log);
		}
		return shader;
	}

	/* Same vertex/fragment pair the OpenGL branch used, retargeted to GLSL 330
	 * (the context is core 3.3).  The fragment shader is a plain texture fetch:
	 * the two phase frames are already quantised by the filter plugin, so the
	 * shader must not do any bit math of its own. */
	void CreateQuadResources()
	{
		const char* vertexShaderSource = R"(
			#version 330 core
			layout (location = 0) in vec2 aPos;
			layout (location = 1) in vec2 aTexCoord;
			out vec2 TexCoord;
			uniform mat4 projection;
			uniform mat4 model;
			/* xy = top-left texel edge of this tile's body, zw = bottom-right
			 * one.  Every tile draws the same unit quad, so the sub-rectangle of
			 * its texture is a uniform instead of a per-tile vertex buffer. */
			uniform vec4 uvRect;
			void main()
			{
				gl_Position = projection * model * vec4(aPos.x, aPos.y, 0.0, 1.0);
				TexCoord = mix(uvRect.xy, uvRect.zw, aTexCoord);
			}
		)";

		const char* fragmentShaderSource = R"(
			#version 330 core
			in vec2 TexCoord;
			out vec4 FragColor;
			uniform sampler2D ourTexture;
			void main()
			{
				FragColor = texture(ourTexture, TexCoord);
			}
		)";

		const GLuint vertexShader   = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
		const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

		shaderProgram_ = glCreateProgram();
		glAttachShader(shaderProgram_, vertexShader);
		glAttachShader(shaderProgram_, fragmentShader);
		glLinkProgram(shaderProgram_);

		GLint success = 0;
		glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &success);
		if (!success)
		{
			char log[1024] = {0};
			glGetProgramInfoLog(shaderProgram_, sizeof(log) - 1, NULL, log);
			glDeleteShader(vertexShader);
			glDeleteShader(fragmentShader);
			throw std::runtime_error(std::string("Shader link failed: ") + log);
		}
		glDeleteShader(vertexShader);
		glDeleteShader(fragmentShader);

		uniformProjection_ = glGetUniformLocation(shaderProgram_, "projection");
		uniformModel_      = glGetUniformLocation(shaderProgram_, "model");
		uniformTexture_    = glGetUniformLocation(shaderProgram_, "ourTexture");
		uniformUvRect_     = glGetUniformLocation(shaderProgram_, "uvRect");

		/* Unit quad, triangle strip.  Texture coordinate v = 0 is the first row
		 * of the uploaded image, which DrawImage() places at the top of the
		 * screen by using a negative y scale.  The uvRect uniform rescales this
		 * 0..1 range onto one tile's body inside its gutter-padded texture. */
		const float positions[]  = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f };
		const float texCoords[]  = { 0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f };

		glGenVertexArrays(1, &vao_);
		glBindVertexArray(vao_);

		glGenBuffers(1, &vboPositions_);
		glBindBuffer(GL_ARRAY_BUFFER, vboPositions_);
		glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

		glGenBuffers(1, &vboTexCoords_);
		glBindBuffer(GL_ARRAY_BUFFER, vboTexCoords_);
		glBufferData(GL_ARRAY_BUFFER, sizeof(texCoords), texCoords, GL_STATIC_DRAW);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

		glBindVertexArray(0);
	}

	/* ── GPU dither: shader resources ──────────────────────────────────────── */

	/*
	 * The fragment shader reproduces FilterCore's quantisers, mode for mode, so
	 * the two backends look the same on screen:
	 *
	 *   0 none             floor(v*255 + 0.5)
	 *   1 ordered          floor(v*255 + rank/16)          (Bayer4x4, no half-step)
	 *   4 bayer4x4         floor(v*255 + (rank+0.5)/16)
	 *   5 bayer8x8         floor(v*255 + (rank+0.5)/64)
	 *   6 bluenoise        floor(v*255 + (rank+0.5)/4096)
	 *   7 spatial_temporal two phases trading a half-code, selected by rank<0.5
	 *   3 pwm              hi/lo chosen by frame index, 2 phases at pwm_bits=1
	 *
	 * Alpha is rounded in every mode, as on the CPU.
	 *
	 * The pattern is indexed by absolute position, exactly like FilterCore: the
	 * vertex shader passes a vImagePos varying carrying image-pixel coordinates
	 * for the tile, so a fragment knows where it sits in the whole picture and a
	 * tile boundary cannot shift the matrix phase.  When the image is minified
	 * the pattern switches to screen coordinates instead -- dithering at image
	 * resolution and then minifying would turn the pattern into aliasing noise.
	 */
	void CreateDitherResources()
	{
		const char* vertexSource = R"(
			#version 330 core
			layout (location = 0) in vec2 aPos;
			layout (location = 1) in vec2 aTexCoord;
			out vec2 TexCoord;
			out vec2 vImagePos;
			uniform mat4 projection;
			uniform mat4 model;
			uniform vec4 uvRect;     /* body top-left / bottom-right, per tile */
			uniform vec4 uTileRect;  /* xy = tile origin px, zw = tile size px */
			void main()
			{
				gl_Position = projection * model * vec4(aPos.x, aPos.y, 0.0, 1.0);
				TexCoord  = mix(uvRect.xy, uvRect.zw, aTexCoord);
				vImagePos = uTileRect.xy + aTexCoord * uTileRect.zw;
			}
		)";

		const char* fragmentSource = R"(
			#version 330 core
			in vec2 TexCoord;
			in vec2 vImagePos;
			out vec4 FragColor;

			uniform sampler2D uSource;
			uniform sampler2D uBlueNoise;   /* 64x64 R16, rank normalised to 0..1 */
			uniform int  uMode;
			uniform int  uPhase;
			uniform int  uComponents;
			uniform int  uDitherSpace;      /* 0 = image pixels, 1 = screen pixels */
			uniform uint uSeed;

			/* Ranks, not thresholds: the half-step is applied at the use site so
			 * mode 1 (which has no half-step, matching FilterCore) and mode 4
			 * (which has one) can share the same table. */
			const float kBayer4[16] = float[16](
				 0.0,  8.0,  2.0, 10.0,
				12.0,  4.0, 14.0,  6.0,
				 3.0, 11.0,  1.0,  9.0,
				15.0,  7.0, 13.0,  5.0);

			const float kBayer8[64] = float[64](
				 0.0, 32.0,  8.0, 40.0,  2.0, 34.0, 10.0, 42.0,
				48.0, 16.0, 56.0, 24.0, 50.0, 18.0, 58.0, 26.0,
				12.0, 44.0,  4.0, 36.0, 14.0, 46.0,  6.0, 38.0,
				60.0, 28.0, 52.0, 20.0, 62.0, 30.0, 54.0, 22.0,
				 3.0, 35.0, 11.0, 43.0,  1.0, 33.0,  9.0, 41.0,
				51.0, 19.0, 59.0, 27.0, 49.0, 17.0, 57.0, 25.0,
				15.0, 47.0,  7.0, 39.0, 13.0, 45.0,  5.0, 37.0,
				63.0, 31.0, 55.0, 23.0, 61.0, 29.0, 53.0, 21.0);

			/* Threshold for pixel p in (0, 1), matching FilterCore::SpatialThreshold. */
			float Threshold(ivec2 p)
			{
				if (uMode == 1)
					return kBayer4[(p.y & 3) * 4 + (p.x & 3)] / 16.0;
				if (uMode == 4)
					return (kBayer4[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
				if (uMode == 5)
					return (kBayer8[(p.y & 7) * 8 + (p.x & 7)] + 0.5) / 64.0;

				ivec2 q = ivec2((p.x + int(uSeed & 63u)) & 63,
				                (p.y + int((uSeed >> 6) & 63u)) & 63);
				float rank = floor(texelFetch(uBlueNoise, q, 0).r * 65535.0 + 0.5);
				return (rank + 0.5) / 4096.0;
			}

			float QuantizeBayer(float v, float s)
			{
				return min(255.0, floor(clamp(v, 0.0, 1.0) * 255.0 + s)) / 255.0;
			}

			float QuantizeRound(float v)
			{
				return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
			}

			/* FilterCore::ApplySpatialTemporal, per channel: the pair trades one
			 * half-code, and the frame index decides which phase shows it. */
			float QuantizeTemporal(float v, float s)
			{
				float q9     = floor(clamp(v, 0.0, 1.0) * 510.0 + 0.5);
				float base   = floor(q9 * 0.5);
				bool  odd    = mod(q9, 2.0) > 0.5;
				bool  pHigh  = s < 0.5;
				bool  showHi = (uPhase == 0) ? pHigh : !pHigh;
				return (base + ((odd && showHi) ? 1.0 : 0.0)) / 255.0;
			}

			/* FilterCore::ApplyPWM at two phases. */
			float QuantizePwm(float v)
			{
				float scaled  = clamp(v, 0.0, 1.0) * 255.0;
				float lo      = floor(scaled);
				float hi      = min(lo + 1.0, 255.0);
				float hiCount = floor((scaled - lo) * 2.0 + 0.5);
				return ((float(uPhase) < hiCount) ? hi : lo) / 255.0;
			}

			void main()
			{
				vec4 t = texture(uSource, TexCoord);
				vec3 rgb;
				float a;
				if (uComponents == 1)      { rgb = t.rrr; a = 1.0; }
				else if (uComponents == 2) { rgb = t.rrr; a = t.g; }
				else if (uComponents == 3) { rgb = t.rgb; a = 1.0; }
				else                       { rgb = t.rgb; a = t.a; }

				ivec2 p = (uDitherSpace == 0) ? ivec2(floor(vImagePos))
				                              : ivec2(floor(gl_FragCoord.xy));
				float s = Threshold(p);

				vec3 outc;
				if (uMode == 0) {
					outc = vec3(QuantizeRound(rgb.r), QuantizeRound(rgb.g), QuantizeRound(rgb.b));
				} else if (uMode == 3) {
					outc = vec3(QuantizePwm(rgb.r), QuantizePwm(rgb.g), QuantizePwm(rgb.b));
				} else if (uMode == 7) {
					outc = vec3(QuantizeTemporal(rgb.r, s), QuantizeTemporal(rgb.g, s),
					            QuantizeTemporal(rgb.b, s));
				} else {
					outc = vec3(QuantizeBayer(rgb.r, s), QuantizeBayer(rgb.g, s),
					            QuantizeBayer(rgb.b, s));
				}
				FragColor = vec4(outc, QuantizeRound(a));
			}
		)";

		try
		{
			const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSource);
			GLuint fs = 0;
			try { fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSource); }
			catch (...) { glDeleteShader(vs); throw; }

			ditherProgram_ = glCreateProgram();
			glAttachShader(ditherProgram_, vs);
			glAttachShader(ditherProgram_, fs);
			glLinkProgram(ditherProgram_);
			glDeleteShader(vs);
			glDeleteShader(fs);

			GLint linked = 0;
			glGetProgramiv(ditherProgram_, GL_LINK_STATUS, &linked);
			if (!linked)
			{
				char log[1024] = {0};
				glGetProgramInfoLog(ditherProgram_, sizeof(log) - 1, NULL, log);
				glDeleteProgram(ditherProgram_);
				ditherProgram_ = 0;
				throw std::runtime_error(std::string("Dither shader link failed: ") + log);
			}
		}
		catch (const std::exception& ex)
		{
			/* A driver that refuses the shader is not fatal: the CPU backend is
			 * the default and still works, so report and leave ditherReady_ false. */
			std::fprintf(stderr, "iview: GPU filter unavailable (%s); using cpu\n", ex.what());
			IviewTrace("dither shader unavailable");
			return;
		}

		ditherProjection_ = glGetUniformLocation(ditherProgram_, "projection");
		ditherModel_      = glGetUniformLocation(ditherProgram_, "model");
		ditherUvRect_     = glGetUniformLocation(ditherProgram_, "uvRect");
		ditherTileRect_   = glGetUniformLocation(ditherProgram_, "uTileRect");
		ditherSource_     = glGetUniformLocation(ditherProgram_, "uSource");
		ditherNoise_      = glGetUniformLocation(ditherProgram_, "uBlueNoise");
		ditherMode_       = glGetUniformLocation(ditherProgram_, "uMode");
		ditherPhase_      = glGetUniformLocation(ditherProgram_, "uPhase");
		ditherComponents_ = glGetUniformLocation(ditherProgram_, "uComponents");
		ditherSpace_      = glGetUniformLocation(ditherProgram_, "uDitherSpace");
		ditherSeed_       = glGetUniformLocation(ditherProgram_, "uSeed");

		/* The 64x64 blue-noise tile, ranks 0..4095 stored as R16 normalised.
		 * 8 KiB, built once: the pattern must not change between frames or the
		 * dither would shimmer as it is re-uploaded. */
		glGenTextures(1, &blueNoiseTex_);
		glBindTexture(GL_TEXTURE_2D, blueNoiseTex_);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, 64, 64, 0,
		             GL_RED, GL_UNSIGNED_SHORT, kBlueNoise64);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glBindTexture(GL_TEXTURE_2D, 0);

		ditherReady_ = true;
		IviewTrace("dither shader ready");
	}

	/* Reset from the unscaled base on every change: ScaleAllSizes() is
	 * cumulative, so calling it repeatedly would grow the UI. */
	void ApplyUiScale(float scale)
	{
		if (!imgui_) return;
		const float clamped = std::clamp(scale, Config::kMinUiScale, Config::kMaxUiScale);
		ImGuiStyle style = imgui_->baseStyle;
		style.Alpha = ImGui::GetStyle().Alpha;
		style.ScaleAllSizes(clamped);
		style.FontScaleMain = clamped;   /* text; GetFontSize() = FontSizeBase * this */
		ImGui::GetStyle() = style;
		settings_.uiScale = clamped;
	}

	void InitImGui()
	{
		imgui_ = std::make_unique<WindowImGuiState>();
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;   /* no imgui.ini next to the executable */
		io.LogFilename = nullptr;
		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding   = 0.0f;
		style.WindowBorderSize = 0.0f;
		style.FrameRounding    = 3.0f;

		/* Remember the unscaled style before applying settings_.uiScale. */
		imgui_->baseStyle = style;
		ApplyUiScale(settings_.uiScale);

		/* install_callbacks = true chains the GLFW callbacks installed above,
		 * including the file drop callback. */
		if (!ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true) ||
		    !ImGui_ImplOpenGL3_Init("#version 330 core")) {
			ImGui_ImplOpenGL3_Shutdown();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			[[unlikely]] throw std::runtime_error("Failed to initialise ImGui OpenGL3 backend");
		}
		imgui_->initialized = true;
	}

	static void ShutdownImGui() noexcept
	{
		if (ImGui::GetCurrentContext() == nullptr) return;
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
	}

	/* ── Image → GPU ───────────────────────────────────────────────────────── */

	/* Build an ImagePluginData view of an Image (no pixel data copy). */
	static ImagePluginData MakePluginDataView(const Image& image) noexcept
	{
		ImagePluginData pd;
		pd.width      = image.width;
		pd.height     = image.height;
		pd.stride     = image.stride;
		pd.colorSpace = image.colorSpace;
		pd.size       = image.size;
		pd.data       = image.data;
		pd.format     = image.format;
		/* This is a view: the components belong to the Image, not to `pd`, so
		 * no ownedBlob is attached and both views are pointed at the same
		 * table. */
		iw_plugin_data_bind(pd, image.format.components, image.format.componentCount);
		return pd;
	}

	static bool IsPackedRgba8(const IWImageFormat& f) noexcept
	{
		if (!f.storageLayout.isInterleavedTight()) return false;
		if (f.componentCount != 4u || f.bitsPerPixel != 32u) return false;
		static const IWComponentSemantic kOrder[4] = {
			IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_SEMANTIC_G,
			IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_SEMANTIC_A
		};
		for (int i = 0; i < 4; ++i) {
			const IWComponentDef& c = f.components[i];
			if (c.semantic       != kOrder[i])                return false;
			if (c.componentClass != IW_COMPONENT_CLASS_UNORM) return false;
			if (c.bitWidth       != 8u)                       return false;
			if (c.bitOffset      != (uint16_t)(i * 8))        return false;
		}
		return true;
	}

	static bool IsPackedRgb8(const IWImageFormat& f) noexcept
	{
		if (!f.storageLayout.isInterleavedTight()) return false;
		if (f.componentCount != 3u || f.bitsPerPixel != 24u) return false;
		static const IWComponentSemantic kOrder[3] = {
			IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_SEMANTIC_B
		};
		for (int i = 0; i < 3; ++i) {
			const IWComponentDef& c = f.components[i];
			if (c.semantic       != kOrder[i])                return false;
			if (c.componentClass != IW_COMPONENT_CLASS_UNORM) return false;
			if (c.bitWidth       != 8u)                       return false;
			if (c.bitOffset      != (uint16_t)(i * 8))        return false;
		}
		return true;
	}

	static bool IsPackedGray8(const IWImageFormat& f) noexcept
	{
		return f.storageLayout.isInterleavedTight() &&
		       f.componentCount == 1u && f.bitsPerPixel == 8u &&
		       f.components[0].semantic       == IW_COMPONENT_SEMANTIC_GRAY &&
		       f.components[0].componentClass == IW_COMPONENT_CLASS_UNORM &&
		       f.components[0].bitWidth       == 8u &&
		       f.components[0].bitOffset      == 0u;
	}

	/* Convert one frame to tightly packed RGBA8.  The plugin already produces
	 * exactly that layout, so the common path is a straight row copy.  JPEG and
	 * most loaders hand back packed RGB8 or GRAY8; those get integer expansion
	 * here instead of the generic float path, which used to run
	 * FilterCore::ExtractPixelF() four times per pixel.  Anything else
	 * (float/16-bit/exotic semantics) goes through the FilterCore helper. */
	static void ToRgba8(const ImagePluginData& pd, std::vector<uint8_t>& out)
	{
		out.resize((size_t)pd.width * (size_t)pd.height * 4u);
		if (IsPackedRgba8(pd.format)) {
			for (int y = 0; y < pd.height; ++y)
				std::memcpy(out.data() + (size_t)y * pd.width * 4u,
				            pd.data  + (ptrdiff_t)y * pd.stride,
				            (size_t)pd.width * 4u);
			return;
		}

		if (IsPackedRgb8(pd.format)) {
			for (int y = 0; y < pd.height; ++y) {
				const uint8_t* src = pd.data + (ptrdiff_t)y * pd.stride;
				uint8_t* dst = out.data() + (size_t)y * pd.width * 4u;
				for (int x = 0; x < pd.width; ++x) {
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
					dst[3] = 255u;
					src += 3;
					dst += 4;
				}
			}
			return;
		}

		if (IsPackedGray8(pd.format)) {
			for (int y = 0; y < pd.height; ++y) {
				const uint8_t* src = pd.data + (ptrdiff_t)y * pd.stride;
				uint8_t* dst = out.data() + (size_t)y * pd.width * 4u;
				for (int x = 0; x < pd.width; ++x) {
					const uint8_t v = src[x];
					dst[0] = v;
					dst[1] = v;
					dst[2] = v;
					dst[3] = 255u;
					dst += 4;
				}
			}
			return;
		}

		for (int y = 0; y < pd.height; ++y) {
			uint8_t* dst = out.data() + (size_t)y * pd.width * 4u;
			for (int x = 0; x < pd.width; ++x) {
				float rgba[4];
				FilterCore::ExtractPixelF(pd, x, y, rgba);
				for (int c = 0; c < 4; ++c) {
					const float v = std::clamp(rgba[c], 0.0f, 1.0f);
					dst[x * 4 + c] = (uint8_t)std::lround(v * 255.0f);
				}
			}
		}
	}

	/* A raster ready for upload: already in a layout GL can take verbatim.
	 * Both upload paths -- an Image's own packed pixels and the filter's RGBA8
	 * frames -- reduce to this, so tiling has exactly one implementation. */
	struct RasterSource
	{
		const uint8_t* data   = nullptr;
		int    width  = 0;
		int    height = 0;
		int    stride = 0;            /* bytes per row           */
		int    bpp    = 0;            /* bytes per pixel         */
		GLenum format = GL_RGBA;      /* GL_RED/RG/RGB/RGBA …    */
		GLint  internalFormat = GL_RGBA8;
		GLenum type   = GL_UNSIGNED_BYTE;
		int    components = 4;        /* channels the shader sees */
	};

	static RasterSource Rgba8Raster(const uint8_t* data, int width, int height)
	{
		RasterSource src;
		src.data           = data;
		src.width          = width;
		src.height         = height;
		src.bpp            = 4;
		src.stride         = width * 4;
		src.format         = GL_RGBA;
		src.internalFormat = GL_RGBA8;
		src.type           = GL_UNSIGNED_BYTE;
		src.components     = 4;
		return src;
	}

	/* Reads GL_MAX_TEXTURE_SIZE once the context is current and derives the tile
	 * body size from it.  Called from the constructor; the limits never change
	 * for the life of the window. */
	void ApplyTextureLimits()
	{
		GLint maxSize = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
		maxTextureSize_ = (maxSize > 0) ? static_cast<int>(maxSize) : 2048;
		const int ceiling = std::min(maxTextureSize_, kPreferredTileExtent);
		tileExtent_ = std::max(1, ceiling - 2 * kTextureBorder);
		if (IviewTraceEnabled())
		{
			char line[128];
			std::snprintf(line, sizeof(line),
			              "texture limits: max_size=%d tile_extent=%d border=%d",
			              maxTextureSize_, tileExtent_, kTextureBorder);
			IviewTrace(line);
		}
	}

	void DeletePass(TexturePass& pass)
	{
		for (TextureTile& tile : pass.tiles)
			if (tile.id != 0) glDeleteTextures(1, &tile.id);
		pass.tiles.clear();
	}

	void DeletePasses()
	{
		DeletePass(passes_[0]);
		DeletePass(passes_[1]);
		passCount_ = 0;
	}

	/* Texel index -> source index along one axis, as a few straight copies.
	 * Inside the raster the mapping is the identity shifted by the gutter; the
	 * two gutter bands outside it repeat the nearest edge pixel, which is what
	 * GL_CLAMP_TO_EDGE gives an untiled upload. */
	struct UploadSpan { int dst; int src; int count; };

	static int BuildSpans(int origin, int extent, int limit, UploadSpan out[3]) noexcept
	{
		int n = 0;
		out[n++] = { 0, std::max(origin - kTextureBorder, 0), kTextureBorder };
		out[n++] = { kTextureBorder, origin, extent };
		out[n++] = { kTextureBorder + extent,
		             std::min(origin + extent, limit - 1), kTextureBorder };
		return n;
	}

	/* Allocates one tile texture and fills it from the source raster.  Every fill
	 * is a glTexSubImage2D straight out of the source buffer: the unpack skip and
	 * row-length state selects the sub-rectangle, so no pixel is copied on the
	 * CPU and no staging buffer exists at any point — the property that made the
	 * old single-texture fast path worth having. */
	void UploadTile(TextureTile& tile, const RasterSource& src)
	{
		tile.texW = tile.w + 2 * kTextureBorder;
		tile.texH = tile.h + 2 * kTextureBorder;

		glGenTextures(1, &tile.id);
		glBindTexture(GL_TEXTURE_2D, tile.id);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, src.internalFormat, tile.texW, tile.texH, 0,
		             src.format, src.type, nullptr);

		UploadSpan xs[3];
		UploadSpan ys[3];
		const int nx = BuildSpans(tile.x0, tile.w, src.width,  xs);
		const int ny = BuildSpans(tile.y0, tile.h, src.height, ys);

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, src.stride / src.bpp);
		for (int j = 0; j < ny; ++j)
		{
			for (int i = 0; i < nx; ++i)
			{
				glPixelStorei(GL_UNPACK_SKIP_ROWS,   ys[j].src);
				glPixelStorei(GL_UNPACK_SKIP_PIXELS, xs[i].src);
				glTexSubImage2D(GL_TEXTURE_2D, 0, xs[i].dst, ys[j].dst,
				                xs[i].count, ys[j].count,
				                src.format, src.type, src.data);
			}
		}
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}

	/* Cuts one raster into tiles and uploads them.  Tile boundaries are source
	 * pixel boundaries, so at 1:1 the texel grid still lands on the screen pixel
	 * grid. */
	void BuildPass(TexturePass& pass, const RasterSource& src)
	{
		const int extent = std::max(1, tileExtent_);
		const int cols   = (src.width  + extent - 1) / extent;
		const int rows   = (src.height + extent - 1) / extent;
		pass.tiles.reserve(static_cast<size_t>(cols) * static_cast<size_t>(rows));
		for (int row = 0; row < rows; ++row)
		{
			for (int col = 0; col < cols; ++col)
			{
				/* Register the tile before it owns a texture, so the failure path
				 * below can never leak one. */
				pass.tiles.push_back(TextureTile{});
				TextureTile& tile = pass.tiles.back();
				tile.x0 = col * extent;
				tile.y0 = row * extent;
				tile.w  = std::min(extent, src.width  - tile.x0);
				tile.h  = std::min(extent, src.height - tile.y0);
				UploadTile(tile, src);
			}
		}
	}

	/* Replaces the on-screen textures.  The new set is built in full before the
	 * old one is deleted, so a failure leaves the previous picture intact. */
	void UploadRasters(const RasterSource* srcs, int count)
	{
		TexturePass built[2];
		try
		{
			for (int p = 0; p < count; ++p) BuildPass(built[p], srcs[p]);
		}
		catch (...)
		{
			DeletePass(built[0]);
			DeletePass(built[1]);
			throw;
		}

		DeletePasses();
		passes_[0] = std::move(built[0]);
		passes_[1] = std::move(built[1]);
		passCount_   = count;
		rasterWidth_  = srcs[0].width;
		rasterHeight_ = srcs[0].height;
		/* Default: these tiles hold display-ready RGBA8, so the shader must not
		 * dither them again.  TryUploadNativeSource() overrides this after the
		 * upload succeeds. */
		sourceIsNative_   = false;
		nativeComponents_ = 4u;
		glBindTexture(GL_TEXTURE_2D, 0);

		if (IviewTraceEnabled())
		{
			char line[192];
			std::snprintf(line, sizeof(line),
			              "texture upload done (%dx%d, passes=%d, tiles=%d+%d, extent=%d)",
			              rasterWidth_, rasterHeight_, passCount_,
			              static_cast<int>(passes_[0].tiles.size()),
			              static_cast<int>(passes_[1].tiles.size()), tileExtent_);
			IviewTrace(line);
		}
	}

	/* One pass per phase frame.  A single-frame source uploads once: the second
	 * phase keeps drawing the same pass, and the previous second-phase textures
	 * (from a PWM run) are simply gone. */
	void UploadTextures(const std::vector<uint8_t>& phaseA,
	                    const std::vector<uint8_t>& phaseB,
	                    int width, int height)
	{
		IviewTrace("texture upload start");
		RasterSource srcs[2];
		srcs[0] = Rgba8Raster(phaseA.data(), width, height);
		const int count = phaseB.empty() ? 1 : 2;
		if (count == 2) srcs[1] = Rgba8Raster(phaseB.data(), width, height);
		UploadRasters(srcs, count);
	}

	/* Fast path for the common 8-bit layouts: upload the Image's own pixels
	 * instead of converting to a separate RGBA8 frame.  A 24 MP RGB image is
	 * 72 MB; converting it to RGBA8 costs a 96 MB allocation plus a full pass
	 * over both buffers, which this avoids for the mode=none display path.
	 * Returns false for layouts this path does not handle (the caller converts). */
	bool TryUploadPackedSource(const Image& image)
	{
		RasterSource src;
		if (IsPackedRgba8(image.format)) {
			src.internalFormat = GL_RGBA8;
			src.format         = GL_RGBA;
		} else if (IsPackedRgb8(image.format)) {
			src.internalFormat = GL_RGB8;
			src.format         = GL_RGB;
		} else {
			return false;
		}

		src.data   = image.data;
		src.width  = image.width;
		src.height = image.height;
		src.stride = image.stride;
		src.bpp    = image.BytesPerPixel();
		if (src.bpp <= 0) return false;

		IviewTrace("direct texture upload start");
		UploadRasters(&src, 1);
		IviewTrace("direct texture upload done");
		return true;
	}

	/* Native-precision upload for the shader dither path.
	 *
	 * The CPU backend uploads finished RGBA8 frames; the GPU backend uploads the
	 * source as it was decoded, so the shader still has the sub-8-bit bits a
	 * dither needs.  ClassifyGLUpload() already answers which formats GL can take
	 * verbatim, so this maps its answer to concrete enumerators instead of
	 * re-deriving the rules.
	 *
	 * Returns false when the format needs conversion (planar, mixed precision,
	 * exotic semantics, float16, 16-bit BGR).  The caller then falls back to the
	 * CPU backend, which handles every format the plugin does.
	 *
	 * SNORM/SINT are rejected even though ClassifyGLUpload() accepts them: GL's
	 * normalised integer sampling is unsigned, so uploading signed data as
	 * GL_R16 would rescale it through [0,1] and invert the negative half.  The
	 * CPU path applies the signed mapping explicitly, so it is the correct
	 * backend for those frames. */
	bool TryUploadNativeSource(const Image& image)
	{
		const IWGLUploadHint hint = ClassifyGLUpload(image.format);
		if (!hint.valid) return false;

		for (uint16_t i = 0; i < image.format.componentCount; ++i)
		{
			const IWComponentClass cls = image.format.components[i].componentClass;
			if (cls == IW_COMPONENT_CLASS_SNORM || cls == IW_COMPONENT_CLASS_SINT)
				return false;
		}

		RasterSource src;
		src.data       = image.data;
		src.width      = image.width;
		src.height     = image.height;
		src.stride     = image.stride;
		src.bpp        = image.BytesPerPixel();
		src.components = hint.channels;
		if (src.bpp <= 0) return false;

		const bool wide = hint.isFloat || hint.bitWidth == 16u;
		if (hint.isFloat)              src.type = GL_FLOAT;
		else if (hint.bitWidth == 8u)  src.type = GL_UNSIGNED_BYTE;
		else if (hint.bitWidth == 16u) src.type = GL_UNSIGNED_SHORT;
		else return false;

		switch (hint.channels)
		{
		case 1:
			src.format         = GL_RED;
			src.internalFormat = hint.isFloat ? GL_R32F : (wide ? GL_R16 : GL_R8);
			break;
		case 2:
			src.format         = GL_RG;
			src.internalFormat = hint.isFloat ? GL_RG32F : (wide ? GL_RG16 : GL_RG8);
			break;
		case 3:
			src.internalFormat = hint.isFloat ? GL_RGB32F : (wide ? GL_RGB16 : GL_RGB8);
			/* GL_BGR exists only for byte data; a 16-bit BGR frame would need a
			 * channel swap on the CPU, which is exactly what the cpu backend
			 * already does. */
			if (hint.isBGR) { if (wide) return false; src.format = GL_BGR; }
			else            { src.format = GL_RGB; }
			break;
		default:
			src.internalFormat = hint.isFloat ? GL_RGBA32F : (wide ? GL_RGBA16 : GL_RGBA8);
			if (hint.isBGR) { if (wide) return false; src.format = GL_BGRA; }
			else            { src.format = GL_RGBA; }
			break;
		}

		IviewTrace("native texture upload start");
		UploadRasters(&src, 1);
		sourceIsNative_   = true;
		nativeComponents_ = src.components;
		IviewTrace("native texture upload done");
		return true;
	}

	/* True when the GPU backend is both asked for and able to draw this frame. */
	bool GpuFilterActive() const noexcept
	{
		return filterBackend_ == Config::FilterBackend::Gpu && ditherReady_ && sourceIsNative_;
	}

	/* Modes the dither shader can express.  Floyd-Steinberg is the exception: its
	 * error diffuses across rows, which a per-fragment shader cannot do. */
	static bool ModeSupportedOnGpu(uint32_t mode) noexcept
	{
		return mode != 2u;
	}

	/* ── Filter pipeline: compute (any thread) / upload (main thread) ──────── */

	/* True when the options ask for no filtering at all.  Kept as one predicate
	 * so the upload fast paths and ComputeFilteredFrames cannot disagree. */
	static bool IsNoFilterOptions(const IWFilterOptions& opts) noexcept
	{
		return opts.argc == 0u || opts.argv == nullptr ||
		       (opts.argc == 1u &&
		        opts.argv[0].type == IW_FILTER_ARG_UINT32 &&
		        std::strcmp(opts.argv[0].name, "mode") == 0 &&
		        opts.argv[0].value.u32 == 0u);
	}

	/* Runs the plugin and converts at most two frames to packed RGBA8.
	 *
	 * PURITY: only the image pixels and the plugin DLL are touched — no GL, no
	 * ImGui, no window.  That is what makes the async re-run in
	 * RequestFilterChange() safe.  Callers must guarantee that only one compute
	 * is in flight at a time: the plugin is a single DLL instance with no
	 * documented reentrancy.
	 *
	 * Throws on failure so the caller can keep the previous textures. */
	FilterFrameSet ComputeFilteredFrames(const Image& image, const IWFilterOptions& opts)
	{
		FilterFrameSet fs;
		fs.width  = image.width;
		fs.height = image.height;
		if (image.width <= 0 || image.height <= 0)
			throw std::runtime_error("the image is empty");

		/* mode=none means "no filter".  Calling the filter plugin anyway made it
		 * allocate and copy a full RGBA frame (pixman PIXMAN_OP_SRC) that was then
		 * copied again by ToRgba8 below.  Skip the round trip entirely. */
		const bool noFilter = IsNoFilterOptions(opts);

		if (!noFilter && filterPlugin_.IsLoaded()) {
			const ImagePluginData pd = MakePluginDataView(image);
			/* 8 bits per channel, 4 channels: what the default framebuffer offers. */
			const IWScreenInfo screen{ 8u, 4u };

			IWFilterImageSet* set = filterPlugin_.Filter(&pd, &screen, &opts);
			if (set == nullptr)
				throw std::runtime_error("the filter plugin returned no frame set");

			if (set->frameCount > 2u) {
				const uint32_t frames = set->frameCount;
				filterPlugin_.Free(set);
				throw std::runtime_error("pwm_bits > 1 is not implemented in this build (" +
				                         std::to_string(frames) + " phase frames)");
			}
			for (uint32_t i = 0; i < set->frameCount; ++i) {
				if (set->frames == nullptr || set->frames[i] == nullptr) continue;
				if (i == 0) ToRgba8(*set->frames[i], fs.phaseA);
				else        ToRgba8(*set->frames[i], fs.phaseB);
			}
			filterPlugin_.Free(set);
		}

		if (fs.phaseA.empty()) {
			/* No plugin (or it returned nothing): use the image directly. */
			ToRgba8(MakePluginDataView(image), fs.phaseA);
		}
		return fs;
	}

	/* Main thread only: GL upload of an already computed frame set. */
	void UploadFrameSet(const FilterFrameSet& fs)
	{
		UploadTextures(fs.phaseA, fs.phaseB, fs.width, fs.height);
		imageWidth_  = fs.width;
		imageHeight_ = fs.height;
		if (IviewTraceEnabled())
		{
			char line[128];
			std::snprintf(line, sizeof(line), "uploaded %d phase frame(s), %dx%d",
			              fs.phaseB.empty() ? 1 : 2, fs.width, fs.height);
			IviewTrace(line);
		}
	}

	/* Options list carrying only the mode.  Used for the immediate mode=none
	 * pass, which must not depend on what the command line or config asked for. */
	static IWFilterOptions ModeOnlyOptions(uint32_t mode, IWFilterArg (&storage)[1]) noexcept
	{
		std::memset(storage, 0, sizeof(IWFilterArg));
		std::strncpy(storage[0].name, "mode", IW_FILTER_ARG_NAME_MAX - 1u);
		storage[0].type      = IW_FILTER_ARG_UINT32;
		storage[0].value.u32 = mode;
		return IWFilterOptions{ IW_FILTER_OPTIONS_VERSION, 1u, storage };
	}

	/* Highest bit width among the active source components.  A source that already
	 * fits the 8-bit display has nothing for the temporal stage to dither, so the
	 * deferred compute is skipped for it. */
	static unsigned SourceBits(const Image& image) noexcept
	{
		unsigned bits = 0;
		const uint32_t count = image.format.componentCount;
		for (uint32_t c = 0; c < count; ++c)
			if (image.format.components[c].bitWidth > bits)
				bits = image.format.components[c].bitWidth;
		return bits;
	}

	/* Starts the worker compute for the current filterOptions_.  The caller
	 * guarantees source_image_ is set and that no compute is in flight. */
	void StartFilterCompute()
	{
		filter_running_  = true;
		filterStartTime_ = std::chrono::steady_clock::now();
		filterGeneration_ = file_generation_;
		const std::shared_ptr<const Image> image = source_image_;
		const IWFilterOptions opts = filterOptions_;     /* argv -> filterArgStorage_ */
		const uint64_t generation = filterGeneration_;
		if (IviewTraceEnabled())
		{
			char line[192];
			std::snprintf(line, sizeof(line),
			              "filter compute submit src=%p gen=%llu mode=%u",
			              static_cast<const void*>(image.get()),
			              static_cast<unsigned long long>(generation),
			              filterMode_);
			IviewTrace(line);
		}
		filter_future_ = FilterJob::Start(image, [this, opts, generation](const Image& source) {
			IviewTrace("filter compute worker start");
			try
			{
				FilterFrameSet result = ComputeFilteredFrames(source, opts);
				if (IviewTraceEnabled())
				{
					char line[128];
					std::snprintf(line, sizeof(line), "filter compute worker done gen=%llu",
					              static_cast<unsigned long long>(generation));
					IviewTrace(line);
				}
				return result;
			}
			catch (const std::exception& ex)
			{
				if (IviewTraceEnabled())
				{
					char line[512];
					std::snprintf(line, sizeof(line), "filter compute worker threw: %s", ex.what());
					IviewTrace(line);
				}
				throw;
			}
			catch (...)
			{
				IviewTrace("filter compute worker threw (unknown)");
				throw;
			}
		});
	}

	/* Called once an image is on screen in mode=none: queue the requested mode in
	 * the background so the window never waits on it. */
	void MaybeStartDeferredFilter()
	{
		if (!source_image_ || filter_running_) return;
		if (filterMode_ == 0u) return;                   /* nothing requested */
		/* The shader backend has already been dithering since the upload: the
		 * mode is a uniform, so there is nothing to compute and nothing to wait
		 * for. */
		if (GpuFilterActive() && ModeSupportedOnGpu(filterMode_)) {
			appliedMode_ = filterMode_;
			IviewTrace("deferred filter handled by the shader backend");
			return;
		}
		if (SourceBits(*source_image_) < 9u) {
			IviewTrace("deferred filter skipped (source has < 9 bits/channel)");
			return;
		}
		StartFilterCompute();
	}

	/* Selection changed in the panel.  Kicks a worker compute; the textures and
	 * the picture on screen stay untouched until the result arrives in Main. */
	void RequestFilterChange(uint32_t mode)
	{
		if (filter_running_) return;   /* one plugin call at a time */
		if (mode == filterMode_) return;

		filterMode_    = mode;      /* the selector reflects the click immediately */
		filter_failed_ = false;
		filter_error_.clear();
		RebuildFilterOptions();

		/* Shader backend: the next frame already shows the new mode, so the
		 * switch is free.  Two cases still need a compute: a mode the shader
		 * cannot express (Floyd-Steinberg), and tiles that no longer hold the
		 * source because an earlier CPU run replaced them -- that one is fixed by
		 * re-uploading at native precision, not by computing. */
		if (filterBackend_ == Config::FilterBackend::Gpu && ditherReady_ &&
		    ModeSupportedOnGpu(mode))
		{
			if (!sourceIsNative_ && source_image_)
			{
				try { TryUploadNativeSource(*source_image_); }
				catch (const std::exception& ex) { std::fprintf(stderr, "iview: %s\n", ex.what()); }
			}
			if (GpuFilterActive()) {
				appliedMode_ = mode;
				IviewTrace("filter mode switched in the shader");
				return;
			}
		}

		/* Before an image exists the choice is persisted/queued; the deferred
		 * run starts from MaybeStartDeferredFilter() once an image is uploaded. */
		if (source_image_) StartFilterCompute();
	}

	/* Switch where the dither is computed.
	 *
	 * Going to the GPU needs the tiles to hold the SOURCE again, so this
	 * re-uploads at native precision.  That is a GL upload on the main thread
	 * (~50 ms for 51 MP) and no CPU pass, which is the whole point of the
	 * backend.
	 *
	 * Going back to the CPU keeps the current picture on screen and queues the
	 * plugin run on a worker, so switching back never blocks the UI.  Until the
	 * result lands the source tiles are drawn without dithering, which is the
	 * same picture minus the pattern -- not a wrong image. */
	void SetFilterBackend(Config::FilterBackend backend)
	{
		if (backend == filterBackend_) return;
		if (filter_running_) return;   /* one plugin call at a time */

		if (backend == Config::FilterBackend::Gpu)
		{
			if (!ditherReady_)
			{
				IviewTrace("gpu backend requested but the dither shader is unavailable");
				return;
			}
			if (source_image_ && !sourceIsNative_)
			{
				try
				{
					if (!TryUploadNativeSource(*source_image_))
					{
						IviewTrace("gpu backend refused this format; staying on cpu");
						return;
					}
				}
				catch (const std::exception& ex)
				{
					std::fprintf(stderr, "iview: %s\n", ex.what());
					return;
				}
			}
			filterBackend_ = Config::FilterBackend::Gpu;
			appliedMode_ = ModeSupportedOnGpu(filterMode_) ? filterMode_ : 0u;
			if (!ModeSupportedOnGpu(filterMode_) && source_image_) StartFilterCompute();
		}
		else
		{
			filterBackend_ = Config::FilterBackend::Cpu;
			appliedMode_ = 0u;
			if (source_image_ && filterMode_ != 0u) StartFilterCompute();
		}

		IviewTrace(backend == Config::FilterBackend::Gpu ? "filter backend: gpu"
		                                        : "filter backend: cpu");
	}

	/* ── Folder navigation ─────────────────────────────────────────────────── */

	static bool IsSupportedImage(const std::filesystem::path& p)
	{
		/* Every entry here must also be routed by Image::PluginPathFor, or the
		 * folder scan offers a file the decoder cannot open.  Four are known to be
		 * in that state: .avif, .heic, .heif and .exr have no decoder in this
		 * tree.  They are kept because an HEIF/AVIF/EXR decoder is wanted and
		 * listing them is a visible reminder; opening one fails with "Unknown
		 * file extension" rather than silently skipping it.  Remove an entry here
		 * when a decoder lands, not before -- the two lists move together. */
		static const char* const kExtensions[] = {
			".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".apng", ".bmp", ".dib",
			".tif", ".tiff", ".webp", ".gif", ".avif", ".heic", ".heif", ".hdr",
			".exr", ".pic", ".ppm", ".pgm", ".pbm", ".pnm", ".tga", ".iwi"
		};
		std::string ext = p.extension().string();
		for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (const char* e : kExtensions)
			if (ext == e) return true;
		return false;
	}

	/* First supported image of a folder.  real_main calls this before launching
	 * the async decode, so a directory argument opens its first image. */
public:
	static std::filesystem::path FirstImageInFolder(const std::filesystem::path& dir)
	{
		std::error_code ec;
		std::vector<std::filesystem::path> found;
		for (std::filesystem::directory_iterator it(dir,
				std::filesystem::directory_options::skip_permission_denied, ec), end;
		     !ec && it != end; it.increment(ec))
		{
			std::error_code fec;
			if (!it->is_regular_file(fec) || fec) continue;
			if (IsSupportedImage(it->path())) found.push_back(it->path());
		}
		if (found.empty())
			throw std::runtime_error("no supported images in " + dir.string());
		std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
			return NaturalLess(a.filename().string(), b.filename().string());
		});
		return found.front();
	}
private:

	/* "img2.png" < "img10.png": digit runs compare numerically, the rest
	 * case-insensitively. */
	static bool NaturalLess(const std::string& a, const std::string& b)
	{
		size_t i = 0, j = 0;
		while (i < a.size() && j < b.size())
		{
			const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
			const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
			if (da && db)
			{
				size_t ia = i, jb = j;
				while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) ++ia;
				while (jb < b.size() && std::isdigit(static_cast<unsigned char>(b[jb]))) ++jb;
				size_t sa = i, sb = j;
				while (sa + 1 < ia && a[sa] == '0') ++sa;
				while (sb + 1 < jb && b[sb] == '0') ++sb;
				const size_t la = ia - sa, lb = jb - sb;
				if (la != lb) return la < lb;
				const int cmp = a.compare(sa, la, b, sb, lb);
				if (cmp != 0) return cmp < 0;
				i = ia; j = jb;
				continue;
			}
			const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
			const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[j])));
			if (ca != cb) return ca < cb;
			++i; ++j;
		}
		return a.size() < b.size();
	}

	/* Fills folder_images_ from the directory holding `any_path` and points
	 * folder_index_ at that file.  Throws when the folder has no supported
	 * image. */
	void ScanFolder(const std::filesystem::path& any_path)
	{
		std::error_code ec;
		std::filesystem::path start = any_path;
		if (!std::filesystem::is_directory(start, ec)) start = start.parent_path();
		if (start.empty()) start = std::filesystem::current_path();

		std::vector<std::filesystem::path> found;
		std::error_code dirEc;
		for (std::filesystem::directory_iterator it(start,
				std::filesystem::directory_options::skip_permission_denied, dirEc), end;
		     !dirEc && it != end; it.increment(dirEc))
		{
			std::error_code fec;
			if (!it->is_regular_file(fec) || fec) continue;
			if (IsSupportedImage(it->path())) found.push_back(it->path());
		}
		if (found.empty())
			throw std::runtime_error("no supported images in " + start.string());

		std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
			return NaturalLess(a.filename().string(), b.filename().string());
		});
		folder_images_ = std::move(found);

		folder_index_ = 0;
		std::error_code cec;
		const std::filesystem::path want = std::filesystem::weakly_canonical(any_path, cec);
		if (!cec)
			for (size_t i = 0; i < folder_images_.size(); ++i)
			{
				std::error_code e2;
				if (std::filesystem::weakly_canonical(folder_images_[i], e2) == want) { folder_index_ = i; break; }
			}
	}

	/* Coalescing request: one decode at a time; a newer target only moves
	 * nav_target_index_ and the in-flight result is discarded on arrival. */
	void NavRequest(size_t target)
	{
		if (folder_images_.empty()) return;
		if (target >= folder_images_.size()) target = folder_images_.size() - 1;
		nav_target_index_ = target;
		if (IviewTraceEnabled())
		{
			char line[160];
			std::snprintf(line, sizeof(line),
			              "nav request target=%zu shown=%zu in_flight=%d",
			              target, nav_shown_index_, nav_in_flight_ ? 1 : 0);
			IviewTrace(line);
		}
		if (!nav_in_flight_) StartNavDecode();
	}

	void StartNavDecode()
	{
		nav_in_flight_ = true;
		const size_t idx = nav_target_index_;
		const std::filesystem::path p = folder_images_[idx];
		/* TODO(prefetch): decode idx+1 while the user looks at idx. */
		if (IviewTraceEnabled())
		{
			char line[512];
			std::snprintf(line, sizeof(line), "nav decode submit idx=%zu path=%s",
			              idx, p.filename().string().c_str());
			IviewTrace(line);
		}
		nav_future_ = std::async(std::launch::async, [idx, p] {
			IviewTrace("nav decode worker start");
			try
			{
				Image image = Image::FromFile(p);
				if (IviewTraceEnabled())
				{
					char line[192];
					std::snprintf(line, sizeof(line), "nav decode worker done idx=%zu %dx%d",
					              idx, image.width, image.height);
					IviewTrace(line);
				}
				return DecodeResult{ idx, std::move(image) };
			}
			catch (const std::exception& ex)
			{
				if (IviewTraceEnabled())
				{
					char line[512];
					std::snprintf(line, sizeof(line), "nav decode worker threw: %s", ex.what());
					IviewTrace(line);
				}
				throw;
			}
			catch (...)
			{
				IviewTrace("nav decode worker threw (unknown)");
				throw;
			}
		});
	}

	void NavigateTo(size_t target)
	{
		if (folder_images_.empty()) return;
		if (target >= folder_images_.size()) target = folder_images_.size() - 1;
		NavigateRelativeTo(target);
	}

	void NavigateRelativeTo(size_t target)
	{
		if (target == nav_target_index_ && !nav_in_flight_ && target == nav_shown_index_) return;
		NavRequest(target);
	}

	void NavigateRelative(int delta)
	{
		if (folder_images_.empty()) return;
		const long long raw = static_cast<long long>(nav_target_index_) + delta;
		if (raw < 0 || raw >= static_cast<long long>(folder_images_.size())) return;   /* clamp */
		if (IviewTraceEnabled())
		{
			char line[160];
			std::snprintf(line, sizeof(line),
			              "next requested delta=%d target=%zu shown=%zu",
			              delta, static_cast<size_t>(raw), nav_shown_index_);
			IviewTrace(line);
		}
		NavigateRelativeTo(static_cast<size_t>(raw));
	}

	/* Zoom policy applied after a navigation upload. */
	void ApplyNavZoom()
	{
		const int mode = settings_.navZoomMode;      /* 0 preserve, 1 fit, 2 100 % */
		if (mode == 1) { fitToWindow(); zoom_mode_ = ZoomMode::Fit; return; }
		if (mode == 2) { SetZoom1to1(); return; }
		if (zoom_mode_ == ZoomMode::Manual) { fitToWindow(); zoom_mode_ = ZoomMode::Fit; }
		else if (zoom_mode_ == ZoomMode::Pct100) SetZoom1to1();
		else fitToWindow();
	}

	void SetZoom1to1()
	{
		setZoom(1.0);
		zoom_mode_ = ZoomMode::Pct100;
	}

	/* Rebuilds the plugin argument list from the current scalars. */
	void RebuildFilterOptions()
	{
		std::memset(filterArgStorage_, 0, sizeof(filterArgStorage_));
		uint32_t count = 0;
		auto addU32 = [&](const char* name, uint32_t value) {
			std::strncpy(filterArgStorage_[count].name, name, IW_FILTER_ARG_NAME_MAX - 1u);
			filterArgStorage_[count].type      = IW_FILTER_ARG_UINT32;
			filterArgStorage_[count].value.u32 = value;
			++count;
		};
		addU32("mode", filterMode_);
		if (filterMode_ == 3u) {                        /* PWM */
			addU32("temporal_bits", pwmBits_);
			addU32("pwm_bits",      pwmBits_);          /* legacy name */
		}
		filterOptions_.version = IW_FILTER_OPTIONS_VERSION;
		filterOptions_.argc    = count;
		filterOptions_.argv    = (count != 0u) ? filterArgStorage_ : nullptr;
	}

public:
	/* Initial load and file drop: compute here, upload here.  Kept under its
	 * OpenGL name so the call sites do not change.  `preserveView` keeps the
	 * current zoom/pan when the new raster has the same dimensions as the one on
	 * screen — used when a progressive preview is replaced by the full image. */
	bool LoadTextures(
		const Image& image,
		const IWFilterOptions& opts = IWFilterOptions{
			IW_FILTER_OPTIONS_VERSION, 0u, nullptr },
		const bool preserveView = false)
	{
		IviewTrace("LoadTextures start");
		const int previousWidth  = imageWidth_;
		const int previousHeight = imageHeight_;
		try
		{
			/* Backend choice, in the order that costs the least:
			 *
			 *   1. GPU backend and the source is natively uploadable: publish
			 *      pixels only, and let the shader quantise.  No CPU pass at
			 *      all, which is what makes opening a large 16-bit file fast.
			 *   2. mode=none with an 8-bit packed source: existing fast path.
			 *   3. otherwise: run the plugin and upload its RGBA8 frames.
			 *
			 * A GPU backend that cannot take this format falls through to 2/3
			 * rather than failing, so the choice never costs the user a file. */
			bool uploaded = false;
			if (filterBackend_ == Config::FilterBackend::Gpu && ditherReady_)
			{
				/* A failure here must not poison the setting: the backend stays
				 * "gpu", sourceIsNative_ stays false, and this file simply takes
				 * the same route the cpu backend would.  The next file may well be
				 * a format the shader can take. */
				try { uploaded = TryUploadNativeSource(image); }
				catch (const std::exception& ex) { std::fprintf(stderr, "iview: %s\n", ex.what()); }
				if (!uploaded)
					IviewTrace("native upload unsupported here; this image uses the cpu path");
				else
				{
					imageWidth_  = image.width;
					imageHeight_ = image.height;
					/* The shader is already drawing the requested mode. */
					appliedMode_ = ModeSupportedOnGpu(filterMode_) ? filterMode_ : 0u;
				}
			}

			if (!uploaded)
			{
				if (IsNoFilterOptions(opts) && TryUploadPackedSource(image))
				{
					imageWidth_  = image.width;
					imageHeight_ = image.height;
				}
				else
				{
					UploadFrameSet(ComputeFilteredFrames(image, opts));
				}
				appliedMode_ = 0;      /* the CPU path uploads mode=none */
			}

			if (!(preserveView &&
			      previousWidth  == imageWidth_ &&
			      previousHeight == imageHeight_))
				CenterImage();
			IviewTrace("LoadTextures done");
			return true;
		}
		catch (const std::exception& ex)
		{
			std::fprintf(stderr, "iview: %s\n", ex.what());
			return false;
		}
	}

	/* Upload a reduced-resolution preview but size it like the final image, so
	 * the preview -> full swap changes only the texture, not the on-screen
	 * rectangle.  Always mode=none: a filter run on the preview would be thrown
	 * away moments later. */
	bool LoadPreviewTextures(const Image& image, int displayWidth, int displayHeight)
	{
		IviewTrace("LoadPreviewTextures start");
		try
		{
			IWFilterArg noneArgs[1];
			const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
			/* A preview has nothing below 8 bits worth dithering and is replaced
			 * moments later, so it always takes the cheap path: native upload when
			 * the format allows it, packed 8-bit upload otherwise. */
			bool uploaded = false;
			if (filterBackend_ == Config::FilterBackend::Gpu && ditherReady_)
				uploaded = TryUploadNativeSource(image);
			if (!uploaded && !TryUploadPackedSource(image))
				UploadFrameSet(ComputeFilteredFrames(image, noneOpts));
			imageWidth_  = displayWidth  > 0 ? displayWidth  : image.width;
			imageHeight_ = displayHeight > 0 ? displayHeight : image.height;
			appliedMode_ = 0;
			CenterImage();
			IviewTrace("LoadPreviewTextures done");
			return true;
		}
		catch (const std::exception& ex)
		{
			std::fprintf(stderr, "iview: %s\n", ex.what());
			return false;
		}
	}

	/* Filter options, already merged with the config file by real_main().  The
	 * argument array is copied into the Window, so the caller's storage does not
	 * have to outlive Main().  At start-up this runs before Main(), i.e. before
	 * any image exists; it never computes anything itself. */
	void SetFilterOptions(const IWFilterOptions& opts)
	{
		const uint32_t count = std::min<uint32_t>(opts.argc, 5u);
		for (uint32_t i = 0; i < count; ++i) filterArgStorage_[i] = opts.argv[i];
		filterOptions_.version = opts.version;
		filterOptions_.argc    = count;
		filterOptions_.argv    = (count != 0u) ? filterArgStorage_ : nullptr;

		const uint32_t previousMode = filterMode_;

		/* Remember the scalars, so a later radio-button change can rebuild the
		 * argument list without losing what the command line said. */
		for (uint32_t i = 0; i < count; ++i) {
			const IWFilterArg& arg = filterArgStorage_[i];
			if (arg.type == IW_FILTER_ARG_UINT32) {
				if (std::strcmp(arg.name, "mode") == 0)          filterMode_  = arg.value.u32;
				else if (std::strcmp(arg.name, "temporal_bits") == 0) pwmBits_ = arg.value.u32;
				else if (std::strcmp(arg.name, "pwm_bits") == 0)  pwmBits_    = arg.value.u32;
				else if (std::strcmp(arg.name, "spatial_bits") == 0) spatialBits_ = arg.value.u32;
			} else if (arg.type == IW_FILTER_ARG_UINT64) {
				if (std::strcmp(arg.name, "seed") == 0)           seed_ = arg.value.u64;
			}
		}

		/* Safety net: if this is ever called again once an image is on screen,
		 * a changed mode must go through the background compute.  SetFilterOptions
		 * must never compute synchronously.  Unreachable at start-up, where it
		 * runs before Main() and source_image_ is still empty. */
		if (source_image_ && !filter_running_ && filterMode_ != previousMode &&
		    filterMode_ != appliedMode_)
			StartFilterCompute();
	}

	void CenterImage()
	{
		setPanning(0.0, 0.0);
		if (!HasImage()) return;
		if (!settings_.initialZoomFit) { zoom_ = 1.0; return; }   /* 100 % */
		zoom_ = ZoomScale::Fit(imageWidth_, imageHeight_, width, height);
	}

	/* ── Main loop ─────────────────────────────────────────────────────────── */

	/* Draws the image, one quad per texture tile, for one phase.  The transform is
	 * built in NDC on the CPU, so the shader keeps the same projection*model
	 * pipeline while needing no per-frame buffer updates.
	 *
	 * Two programs share the quad: the plain sampler for the CPU backend, whose
	 * tiles are already quantised, and the dither program for the GPU backend,
	 * whose tiles are the source pixels at native precision. */
	void DrawImage(uint32_t phase) const
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, width, height);
		glClearColor(canvasBg_[0], canvasBg_[1], canvasBg_[2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		if (!HasImage()) return;

		const bool gpu = GpuFilterActive() && ModeSupportedOnGpu(filterMode_);
		/* The shader backend has a single set of tiles (the source); only the CPU
		 * backend can carry two phase frames. */
		const int pass = gpu ? 0 : ((passCount_ <= 1) ? 0 : static_cast<int>(phase & 1u));
		const std::vector<TextureTile>& tiles = passes_[pass].tiles;
		if (tiles.empty()) return;

		/* At 100 % or more show exact pixels, below that filter them.  A preview
		 * raster can be smaller than the image dimensions it is stretched over;
		 * it must stay linearly filtered even at 100 % so it does not look like a
		 * mosaic while the full decode finishes. */
		const double zoom = getZoom();
		const bool exactPixels = zoom >= 1.0 &&
		                         rasterWidth_  == imageWidth_ &&
		                         rasterHeight_ == imageHeight_;
		const GLint magFilter = exactPixels ? GL_NEAREST : GL_LINEAR;

		/* The dither pattern is tied to image pixels whenever a screen pixel is
		 * no larger than an image pixel.  Below that it switches to screen pixels:
		 * dithering at image resolution and then minifying would sample the
		 * pattern away into noise. */
		const int ditherSpace = (zoom >= 1.0) ? 0 : 1;

		const double imgW = imageWidth_ * zoom;
		const double imgH = imageHeight_ * zoom;
		const double imgX = (width - imgW) * 0.5 + panX;
		const double imgY = (height - imgH) * 0.5 + panY;

		/* Destination pixels per source-raster pixel.  The tiles partition the
		 * raster and the quads partition the image rectangle along the same
		 * boundaries, so they abut exactly: no gap, and no overlap that would
		 * blend the seam twice. */
		const double sx = imgW / rasterWidth_;
		const double sy = imgH / rasterHeight_;

		const float identity[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};

		glUseProgram(gpu ? ditherProgram_ : shaderProgram_);
		if (gpu)
		{
			glUniformMatrix4fv(ditherProjection_, 1, GL_FALSE, identity);
			glUniform1i(ditherSource_, 0);
			glUniform1i(ditherNoise_, 1);
			glUniform1i(ditherMode_, static_cast<GLint>(gpu ? filterMode_ : 0u));
			glUniform1i(ditherPhase_, static_cast<GLint>(phase & 1u));
			glUniform1i(ditherComponents_, nativeComponents_);
			glUniform1i(ditherSpace_, ditherSpace);
			glUniform1ui(ditherSeed_, static_cast<GLuint>(seed_));

			/* Unit 1 holds the blue-noise tile for the life of the window. */
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, blueNoiseTex_);
		}
		else
		{
			glUniformMatrix4fv(uniformProjection_, 1, GL_FALSE, identity);
			glUniform1i(uniformTexture_, 0);
		}

		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(vao_);
		for (const TextureTile& tile : tiles)
		{
			const double left   = imgX + tile.x0 * sx;
			const double top    = imgY + tile.y0 * sy;
			const double right  = imgX + (tile.x0 + tile.w) * sx;
			const double bottom = imgY + (tile.y0 + tile.h) * sy;

			/* Column-major model matrix, y negated so image row 0 lands at the
			 * top; the projection stays identity. */
			const float model[16] = {
				(float)(2.0 * (right - left) / width),  0.0f, 0.0f, 0.0f,
				0.0f, (float)(-2.0 * (bottom - top) / height), 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				(float)(2.0 * left / width - 1.0),
				(float)(1.0 - 2.0 * top / height), 0.0f, 1.0f
			};

			/* The body's outer texel edges: the gutter sits outside this range
			 * and is only ever read by the filter, never mapped to a pixel. */
			const float uv[4] = {
				kTextureBorder / (float)tile.texW,
				kTextureBorder / (float)tile.texH,
				(kTextureBorder + tile.w) / (float)tile.texW,
				(kTextureBorder + tile.h) / (float)tile.texH
			};

			glBindTexture(GL_TEXTURE_2D, tile.id);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
			if (gpu)
			{
				/* Absolute image-pixel rect of this tile's body, so the fragment
				 * can index the dither field without a seam at the tile edge. */
				const float tileRect[4] = {
					(float)tile.x0, (float)tile.y0,
					(float)tile.w,  (float)tile.h
				};
				glUniformMatrix4fv(ditherModel_, 1, GL_FALSE, model);
				glUniform4fv(ditherUvRect_, 1, uv);
				glUniform4fv(ditherTileRect_, 1, tileRect);
			}
			else
			{
				glUniformMatrix4fv(uniformModel_, 1, GL_FALSE, model);
				glUniform4fv(uniformUvRect_, 1, uv);
			}
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glBindVertexArray(0);
	}

	/* First tile of the pass a phase draws from; the frame trace uses it to prove
	 * the A/B alternation.  0 when nothing is uploaded. */
	GLuint PhaseTextureId(uint32_t phase) const noexcept
	{
		const int pass = (passCount_ <= 1) ? 0 : static_cast<int>(phase & 1u);
		return passes_[pass].tiles.empty() ? 0u : passes_[pass].tiles[0].id;
	}

	void Main(ProgressiveImageLoad&& imageLoad, std::filesystem::path imagePath)
	{
		using Clock = std::chrono::steady_clock;

		const std::filesystem::path initialPath = imagePath;
		std::shared_ptr<ProgressiveImageState> initialState = std::move(imageLoad.state);
		if (imageLoad.preview_worker.valid())
			initial_preview_worker_ = std::move(imageLoad.preview_worker);
		if (imageLoad.worker.valid())
			initial_worker_ = std::move(imageLoad.worker);

		bool initialPreviewShown = false;
		bool initialComplete     = !initialState;

		bool uploaded = false;
		bool shown    = false;
		bool windowShown = false;
		bool firstLoop = true;
		uint32_t phase = 0;
		int framesAfterUpload = 0;

		const bool trace = IviewTraceEnabled();
		std::vector<double> frameCosts;
		double cpuSumMs = 0.0;
		auto lastSwapEnd = Clock::now();

		while (!glfwWindowShouldClose(glfwWindow))
		{
			const auto frameStart = Clock::now();
			glfwPollEvents();
			/* Keep the persisted placement current without writing on resize ticks. */
			TrackNormalGeometry();

			if (firstLoop) { IviewTrace("first loop iteration"); firstLoop = false; }

			/* A drop before the initial decode (possible with --settings and no
			 * image) has already uploaded textures through HandleDrop(). */
			if (!uploaded && HasImage())
			{
				uploaded = true;
				IviewTrace("image available before initial decode");
			}

			/* (1) Progressive initial load.  The worker publishes a preview first
			 * and the full raster second; this loop uploads each stage as it lands.
			 * Nothing blocks here: until the preview is ready the loop just presents
			 * clear frames (the window is still hidden). */
			if (initialState && !initialComplete)
			{
				bool previewReady = false;
				bool previewDone  = false;
				bool done         = false;
				bool fullExpected = false;
				int  fullW = 0;
				int  fullH = 0;
				std::optional<Image> previewImage;
				std::optional<Image> fullImage;
				std::exception_ptr loadError;
				{
					std::lock_guard<std::mutex> lock(initialState->mutex);
					previewReady = initialState->preview_ready;
					previewDone  = initialState->preview_done;
					done         = initialState->done;
					fullExpected = initialState->full_expected;
					fullW        = initialState->full_width;
					fullH        = initialState->full_height;
					loadError    = initialState->error;
					if (previewReady && !initialPreviewShown && initialState->preview)
					{
						previewImage.emplace(std::move(*initialState->preview));
						initialState->preview.reset();
					}
					if (done && initialState->full)
					{
						fullImage.emplace(std::move(*initialState->full));
						initialState->full.reset();
					}
				}

				/* Preview stage.  Skipped when the full raster already won the race:
				 * showing the sharper image first is strictly better. */
				if (!initialComplete && previewImage && !fullImage)
				{
					const bool sameFile = current_path_.empty() || current_path_ == initialPath;
					if (!sameFile)
					{
						IviewTrace("stale initial preview discarded");
						initialComplete = true;
					}
					else
					{
						try
						{
							const int displayW = (fullW > 0) ? fullW : previewImage->width;
							const int displayH = (fullH > 0) ? fullH : previewImage->height;
							if (!LoadPreviewTextures(*previewImage, displayW, displayH))
								throw std::runtime_error("the preview could not be uploaded");

							SetWindowTitle(imagePath);
							ComputeReadout(displayW, displayH, imagePath);
							current_path_ = imagePath;

							/* Folder context.  A directory argument arrives here as its
							 * first image, so the parent is always the right folder. */
							if (folder_images_.empty())
							{
								try {
									ScanFolder(imagePath);
									nav_shown_index_  = folder_index_;
									nav_target_index_ = folder_index_;
								} catch (const std::exception& ex) {
									folder_images_.clear();       /* stay single-file */
									std::fprintf(stderr, "iview: %s\n", ex.what());
								}
							}

							uploaded = true;
							initialPreviewShown = true;
							IviewTrace("progressive preview uploaded");

							/* A loader that returned a full-resolution raster from the
							 * preview call has nothing sharper to offer. */
							if (!fullExpected)
							{
								source_image_ = std::make_shared<Image>(std::move(*previewImage));
								++file_generation_;
								pendingDeferredFilter_ = true;
								initialComplete = true;
								IviewTrace("progressive load complete (preview was final)");
							}
						}
						catch (std::exception& ex)
						{
#ifdef _WIN32
							MessageBoxA(NULL, ex.what(), "Error", MB_OK | MB_ICONERROR);
#else
							std::cerr << "Failed to process the request : " << ex.what() << std::endl;
#endif
							break;
						}
					}
				}

				/* The full raster is ready before the preview was shown: either the
				 * full decode won the race or the preview decode failed.  Show it now;
				 * a later preview would add nothing. */
				if (!initialComplete && !initialPreviewShown && fullImage)
				{
					const bool sameFile = current_path_.empty() || current_path_ == initialPath;
					if (!sameFile)
					{
						IviewTrace("stale initial full (no preview) discarded");
						initialComplete = true;
					}
					else
					{
						try
						{
							IWFilterArg noneArgs[1];
							const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
							if (!LoadTextures(*fullImage, noneOpts))
								throw std::runtime_error("the full image could not be uploaded");

							SetWindowTitle(imagePath);
							ComputeReadout(fullImage->width, fullImage->height, imagePath);
							current_path_ = imagePath;
							if (folder_images_.empty())
							{
								try {
									ScanFolder(imagePath);
									nav_shown_index_  = folder_index_;
									nav_target_index_ = folder_index_;
								} catch (const std::exception& ex) {
									folder_images_.clear();
									std::fprintf(stderr, "iview: %s\n", ex.what());
								}
							}
							uploaded = true;
							initialPreviewShown = true;
							source_image_ = std::make_shared<Image>(std::move(*fullImage));
							++file_generation_;
							pendingDeferredFilter_ = true;
							initialComplete = true;
							IviewTrace("progressive full uploaded (first stage)");
						}
						catch (std::exception& ex)
						{
#ifdef _WIN32
							MessageBoxA(NULL, ex.what(), "Error", MB_OK | MB_ICONERROR);
#else
							std::cerr << "Failed to process the request : " << ex.what() << std::endl;
#endif
							break;
						}
					}
				}

				/* Full stage.  Uploading here on the main thread keeps all GL calls
				 * on one thread; the decode itself already happened on the worker. */
				if (!initialComplete && initialPreviewShown && done)
				{
					if (fullImage)
					{
						if (current_path_ != initialPath)
						{
							IviewTrace("stale initial full discarded");
						}
						else
						{
							try
							{
								IWFilterArg noneArgs[1];
								const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
								/* The preview already sized the quad like the full image,
								 * so the view (zoom and pan) survives the swap. */
								if (!LoadTextures(*fullImage, noneOpts, /*preserveView=*/true))
									throw std::runtime_error("the full image could not be uploaded");

								source_image_ = std::make_shared<Image>(std::move(*fullImage));
								++file_generation_;
								ComputeReadout(source_image_->width, source_image_->height, initialPath);
								pendingDeferredFilter_ = true;
								IviewTrace("progressive full uploaded");
							}
							catch (const std::exception& ex)
							{
								last_error_ = ex.what();
								std::fprintf(stderr, "iview: progressive full upload failed: %s\n", ex.what());
							}
						}
					}
					else if (loadError)
					{
						/* The sharper stage failed; keep showing the preview. */
						try { std::rethrow_exception(loadError); }
						catch (const std::exception& ex)
						{
							last_error_ = std::string("Failed: ") + ex.what();
							std::fprintf(stderr, "iview: progressive full decode failed: %s\n", ex.what());
						}
						catch (...) { last_error_ = "Failed: unknown error"; }
					}
					initialComplete = true;
				}

				/* Nothing at all could be decoded.  previewDone matters: the full
				 * worker may fail while the preview is still decoding, and a late
				 * preview is still worth showing. */
				if (!initialComplete && done && previewDone &&
				    !initialPreviewShown && !fullImage && loadError)
				{
					try { std::rethrow_exception(loadError); }
					catch (std::exception& ex)
					{
#ifdef _WIN32
						MessageBoxA(NULL, ex.what(), "Error", MB_OK | MB_ICONERROR);
#else
						std::cerr << "Failed to process the request : " << ex.what() << std::endl;
#endif
					}
					break;
				}

				/* Release our reference once this load is fully consumed.  A worker
				 * still finishing an abandoned load keeps the state alive through its
				 * own shared_ptr, so this only drops the viewer's claim on it. */
				if (initialComplete)
					initialState.reset();
			}

			/* (1a) Navigation decode finished.  Everything happens on this thread;
			 * the old image stayed on screen while the worker decoded. */
			if (nav_in_flight_ && nav_future_.valid() &&
			    nav_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				IviewTrace("nav future ready");
				nav_in_flight_ = false;
				try
				{
					DecodeResult r = nav_future_.get();
					if (r.index == nav_target_index_)
					{
						IWFilterArg noneArgs[1];
						const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
						if (!LoadTextures(r.image, noneOpts))
							throw std::runtime_error("the filter produced an unsupported frame count");

						const std::filesystem::path& shownPath = folder_images_[r.index];
						SetWindowTitle(shownPath);
						ComputeReadout(r.image, shownPath);
						if (IviewTraceEnabled())
						{
							char line[224];
							std::snprintf(line, sizeof(line),
							              "nav upload replacing source old=%p gen=%llu target=%zu filter_running=%d",
							              static_cast<const void*>(source_image_.get()),
							              static_cast<unsigned long long>(file_generation_),
							              r.index, filter_running_ ? 1 : 0);
							IviewTrace(line);
						}
						source_image_ = std::make_shared<Image>(std::move(r.image));
						current_path_ = shownPath;
						++file_generation_;
						nav_shown_index_ = r.index;
						folder_index_    = r.index;
						last_error_.clear();
						ApplyNavZoom();
						pendingDeferredFilter_ = true;
						IviewTrace("nav image uploaded");
					}
					else if (nav_target_index_ != nav_shown_index_)
					{
						/* The user moved on while this decode ran. */
						StartNavDecode();
					}
				}
				catch (const std::exception& ex)
				{
					last_error_ = "Failed: " + folder_images_[nav_target_index_].filename().string();
					std::fprintf(stderr, "iview: nav decode failed: %s\n", ex.what());
				}
			}

			/* (1b) A filter re-run finished: upload it on this thread.  The old
			 * textures kept rendering until this moment, so the picture never
			 * blanked and the phase alternation never stopped. */
			if (filter_running_ && filter_future_.valid() &&
			    filter_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				IviewTrace("filter future ready");
				if (IviewTraceEnabled())
				{
					char line[192];
					std::snprintf(line, sizeof(line),
					              "filter result gen=%llu current_gen=%llu",
					              static_cast<unsigned long long>(filterGeneration_),
					              static_cast<unsigned long long>(file_generation_));
					IviewTrace(line);
				}
				const double computeMs = std::chrono::duration<double, std::milli>(
					Clock::now() - filterStartTime_).count();
				try
				{
					FilterFrameSet fs = filter_future_.get();
					if (!FilterJob::IsCurrentFilterResult(filterGeneration_, file_generation_))
					{
						/* The user navigated while this was computing: the result
						 * belongs to a file that is no longer on screen. */
						IviewTrace("stale filter result discarded");
					}
					else
					{
						IviewTrace("filter upload start");
						const auto uploadStart = Clock::now();
						UploadFrameSet(fs);
						IviewTrace("filter upload done");
						appliedMode_ = filterMode_;
						const double uploadMs = std::chrono::duration<double, std::milli>(
							Clock::now() - uploadStart).count();

						filter_failed_ = false;
						filter_error_.clear();
						if (trace)
						{
							std::fprintf(stderr, "IVIEW_FILTER compute=%.1f ms upload=%.1f ms\n",
							             computeMs, uploadMs);
							std::fflush(stderr);
						}
					}
				}
				catch (const std::exception& ex)
				{
					/* Keep the previous textures; the panel shows the error. */
					filter_failed_ = true;
					filter_error_  = ex.what();
					std::fprintf(stderr, "iview: filter failed: %s\n", ex.what());
					std::fflush(stderr);
				}
				filter_running_ = false;
			}

			/* (2) The visible path.  Settings is deliberately outside
			 * BuildChrome() so chrome fade/Tab cannot hide it, and it can be
			 * present before the first image. */
			bool uiActive = uploaded || settings_open_ || windowShown;
			if (uiActive)
			{
				DrawImage(phase);   /* clears the canvas when no image exists */
				if (uploaded && framesAfterUpload == 0) IviewTrace("first frame drawn");

				ImGui_ImplOpenGL3_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();

				/* Ctrl+, must work while the chrome is hidden and while only
				 * Settings is visible. */
				if (ImGui::IsKeyPressed(ImGuiKey_Comma) && ImGui::GetIO().KeyCtrl)
				{
					if (!settings_open_) OpenSettings(SettingsTab::Interface, true);
					else                 settings_focus_requested_ = true;
				}

				if (uploaded) BuildChrome();
				BuildSettingsWindow();

				ImGui::Render();
				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			}
			else
			{
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glViewport(0, 0, width, height);
				glClearColor(canvasBg_[0], canvasBg_[1], canvasBg_[2], 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}

			/* (3) vsync: blocks until the next refresh, and is the only pacer. */
			glfwSwapBuffers(glfwWindow);
			if (IviewTraceEnabled())
			{
				char line[128];
				std::snprintf(line, sizeof(line),
				              "swap uploaded=%d phase=%u frames=%d",
				              uploaded ? 1 : 0, phase, framesAfterUpload);
				IviewTrace(line);
			}

			if (uploaded)
			{
				if (framesAfterUpload == 0) IviewTrace("first swap");

				/* First ten frames: prove the A/B alternation (0,1,0,1,…). */
				if (framesAfterUpload < 10)
				{
					const unsigned textureId = PhaseTextureId(phase);
					std::fprintf(stderr, "IVIEW_FRAME %d phase=%u texture=%u\n",
					             framesAfterUpload, phase & 1u, textureId);
					std::fflush(stderr);
				}
				++framesAfterUpload;
				phase ^= 1u;
			}

			/* (4) The image is already in the frame that was just swapped in;
			 * only then does the window normally become visible.  Settings may
			 * be explicitly opened before an image exists, in which case the
			 * window is shown for Settings only. */
			if (uploaded && !shown)
			{
				ShowInitialWindow();
				shown = true;
				windowShown = true;
				IviewTrace("glfwShowWindow");
			}
			if (settings_open_ && !windowShown)
			{
				ShowInitialWindow();
				windowShown = true;
				IviewTrace("glfwShowWindow (settings)");
			}

			/* Now that the plain image is on screen, queue the requested filter
			 * mode.  The worker's result is uploaded by block (1b) when ready. */
			if (pendingDeferredFilter_)
			{
				pendingDeferredFilter_ = false;
				MaybeStartDeferredFilter();
			}

			/* Continuous-render cost — measured only after the window is visible,
			 * so the interval reflects the vsync-paced loop rather than the
			 * unthrottled hidden phase. Reported, not optimised. */
			if (trace && shown && framesAfterUpload >= 60 && frameCosts.size() < 120)
			{
				const auto frameEnd = Clock::now();
				cpuSumMs += std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
				frameCosts.push_back(
					std::chrono::duration<double, std::milli>(frameEnd - lastSwapEnd).count());
				lastSwapEnd = frameEnd;

				if (frameCosts.size() == 120)
				{
					std::vector<double> sorted = frameCosts;
					std::sort(sorted.begin(), sorted.end());
					std::fprintf(stderr,
						"IVIEW_TRACE 120 visible frames: median interval %.2f ms "
						"(min %.2f, max %.2f), CPU per frame %.3f ms\n",
						sorted[sorted.size() / 2], sorted.front(), sorted.back(),
						cpuSumMs / 120.0);
					std::fflush(stderr);
				}
			}
		}

		IviewTrace("main loop exit");
		/* Save-on-exit only when something changed.  Placement is captured once
		 * here, from the last normal-state geometry, so a maximized window keeps
		 * both its maximized flag and the rect it un-maximizes to. */
		if (CaptureWindowGeometry()) settings_dirty_ = true;
		if (settings_dirty_) SaveSettings();
		ShutdownImGui();
	}

private:
	/* Restores the stored placement on the still-hidden window.  Order matters:
	 * the normal rect is applied first and the maximize request comes second, so
	 * the rect is what the window restores to when it is un-maximized. */
	WindowPlacement ApplyInitialPlacement(const Config::Settings& settings)
	{
		FrameExtents frame;
		glfwGetWindowFrameSize(glfwWindow, &frame.left, &frame.top, &frame.right, &frame.bottom);

		WindowPlacement resolved;
		resolved.width  = settings.windowWidth;
		resolved.height = settings.windowHeight;
		ClampSizeToDisplays(resolved.width, resolved.height, frame);

		const bool haveSaved = settings.windowX != Config::kWindowPositionUnset &&
		                       settings.windowY != Config::kWindowPositionUnset;
		const WindowPlacement saved{settings.windowX, settings.windowY,
		                            resolved.width, resolved.height};
		resolved = (haveSaved && PlacementIsReachable(saved, frame))
			? saved : CentreOnPrimary(resolved.width, resolved.height, frame);

		if (resolved.width != settings.windowWidth || resolved.height != settings.windowHeight)
			glfwSetWindowSize(glfwWindow, resolved.width, resolved.height);
		/* Wayland does not let a client place its own toplevel; the call would only
		 * emit GLFW_FEATURE_UNAVAILABLE. */
		if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND)
			glfwSetWindowPos(glfwWindow, resolved.x, resolved.y);

		/* GLFW emulates maximize for a hidden window (Win32: WS_MAXIMIZE +
		 * work-area rect).  ShowInitialWindow() then turns that into a real
		 * maximized show with the restore rect attached. */
		if (settings.windowMaximized) glfwMaximizeWindow(glfwWindow);
		return resolved;
	}

	/* Wayland gives clients no global window position: glfwGetWindowPos() reports
	 * (0,0) there, and glfwSetWindowPos() is a no-op.  Recording that (0,0) would
	 * plant a bogus position that a later X11 session would happily honour, so the
	 * position is simply not captured on Wayland — size and maximized state still
	 * are. */
	static bool PlatformCanPlaceWindows() noexcept
	{
		return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
	}

	/* GLFW has no API for the restore rectangle of a maximized window, so the
	 * normal geometry is sampled every frame while the window is normal.  Three
	 * cheap queries, skipped outright when persistence is disabled. */
	void TrackNormalGeometry()
	{
		if (!settings_.rememberWindowSize) return;
		if (glfwGetWindowAttrib(glfwWindow, GLFW_MAXIMIZED) == GLFW_TRUE) return;
		if (glfwGetWindowAttrib(glfwWindow, GLFW_ICONIFIED) == GLFW_TRUE) return;

		int x = 0, y = 0, w = 0, h = 0;
		glfwGetWindowPos(glfwWindow, &x, &y);
		glfwGetWindowSize(glfwWindow, &w, &h);
		if (w <= 0 || h <= 0) return;
		if (PlatformCanPlaceWindows())
		{
			normalX_ = x;
			normalY_ = y;
		}
		normalW_ = w;
		normalH_ = h;
		normalGeometryKnown_ = true;
	}

	/* Folds the live placement into settings_.  Returns true when a value
	 * changed, so the caller can mark the config dirty. */
	bool CaptureWindowGeometry()
	{
		if (!settings_.rememberWindowSize) return false;
		/* A minimized window reports neither its normal rect nor its maximized
		 * state reliably; keep the last good sample instead of overwriting it. */
		if (glfwGetWindowAttrib(glfwWindow, GLFW_ICONIFIED) == GLFW_TRUE) return false;

		const bool maximized = glfwGetWindowAttrib(glfwWindow, GLFW_MAXIMIZED) == GLFW_TRUE;
		TrackNormalGeometry();

		bool changed = false;
		if (settings_.windowMaximized != maximized)
		{
			settings_.windowMaximized = maximized;
			changed = true;
		}
		if (normalGeometryKnown_)
		{
			const bool positionable = PlatformCanPlaceWindows();
			if (settings_.windowWidth  != normalW_ ||
			    settings_.windowHeight != normalH_ ||
			    (positionable && (settings_.windowX != normalX_ || settings_.windowY != normalY_)))
			{
				if (positionable)
				{
					settings_.windowX = normalX_;
					settings_.windowY = normalY_;
				}
				settings_.windowWidth  = normalW_;
				settings_.windowHeight = normalH_;
				changed = true;
			}
		}
		return changed;
	}

	/* Shows the window, maximized when it was left maximized.  On Windows the
	 * show is folded into SetWindowPlacement so Windows records the normal rect
	 * that belongs with the maximized state: GLFW's own "maximize a hidden
	 * window" path only emulates the maximized look (WS_MAXIMIZE + work-area
	 * rect) and leaves that bookkeeping alone, which makes a later un-maximize
	 * land on a guessed rect. */
	void ShowInitialWindow()
	{
#ifdef _WIN32
		if (glfwGetWindowAttrib(glfwWindow, GLFW_MAXIMIZED) == GLFW_TRUE &&
		    normalGeometryKnown_)
		{
			HWND const hwnd = glfwGetWin32Window(glfwWindow);
			WINDOWPLACEMENT placement{};
			placement.length = sizeof(placement);
			if (hwnd != nullptr && GetWindowPlacement(hwnd, &placement) != 0)
			{
				int left = 0, top = 0, right = 0, bottom = 0;
				glfwGetWindowFrameSize(glfwWindow, &left, &top, &right, &bottom);
				/* rcNormalPosition is a frame rect; the viewer stores client coords. */
				placement.showCmd = SW_SHOWMAXIMIZED;
				placement.rcNormalPosition.left   = normalX_ - left;
				placement.rcNormalPosition.top    = normalY_ - top;
				placement.rcNormalPosition.right  = normalX_ + normalW_ + right;
				placement.rcNormalPosition.bottom = normalY_ + normalH_ + bottom;
				if (SetWindowPlacement(hwnd, &placement) != 0) return;
			}
		}
#endif
		glfwShowWindow(glfwWindow);
	}

	void SetWindowTitle(const std::filesystem::path& path)
	{
		const std::string title = "ImageViewer \xE2\x80\x94 " + Utf8Filename(path);
		glfwSetWindowTitle(glfwWindow, title.c_str());
	}

	void ComputeReadout(int imageWidth, int imageHeight, const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
		if (ec)
		{
			readout_dims_ = std::format("{}\xC3\x97{}", imageWidth, imageHeight);
		}
		else
		{
			const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
			readout_dims_ = std::format("{}\xC3\x97{} \xC2\xB7 {:.1f} MB",
			                              imageWidth, imageHeight, megabytes);
		}
	}

	void ComputeReadout(const Image& image, const std::filesystem::path& path)
	{
		ComputeReadout(image.width, image.height, path);
	}

	static std::string Utf8Filename(const std::filesystem::path& path)
	{
		const std::u8string utf8 = path.filename().u8string();
		return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}

	/* Fade + chrome.  Alpha slews 0 -> 1 in 200 ms, idles at 0.15 after 1.5 s
	 * without mouse motion, and drops to 0 when Tab hides the chrome. */
	void BuildChrome()
	{
		using Clock = std::chrono::steady_clock;
		const auto now = Clock::now();
		++chromeFrames_;

		if (!imgui_->frameClockStarted)
		{
			imgui_->lastMouseActivity = now;
			imgui_->frameClockStarted = true;
		}

		const ImGuiIO& io = ImGui::GetIO();
		if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
			imgui_->lastMouseActivity = now;

		const bool idle = (now - imgui_->lastMouseActivity) > std::chrono::milliseconds(fadeDelayMs_);
		float target = fadeMinAlpha_;
		if (!imgui_->chromeEnabled)      target = 0.0f;
		else if (!idle)                  target = 1.0f;

		const float step = (io.DeltaTime > 0.0f) ? (io.DeltaTime / 0.2f) : 1.0f;
		if (imgui_->alpha < target) imgui_->alpha = std::min(target, imgui_->alpha + step);
		else                        imgui_->alpha = std::max(target, imgui_->alpha - step);

		ImGui::GetStyle().Alpha = imgui_->alpha;
		if (imgui_->alpha <= 0.001f) return;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		constexpr ImGuiWindowFlags kChromeFlags =
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize;

		if (settings_.showReadout)
		{
			/* Top-right readout: dimensions and on-disk size, computed once. */
			ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 24.0f, vp->WorkPos.y + 20.0f),
			                        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
			if (ImGui::Begin("##readout", nullptr, kChromeFlags | ImGuiWindowFlags_NoInputs))
			{
				/* States compose: counter, size, dithering, error. */
				char counter[96] = {0};
				std::string text;
				if (!last_error_.empty())
				{
					text = last_error_;
				}
				else if (nav_in_flight_)
				{
					std::snprintf(counter, sizeof(counter), "Loading %zu / %zu",
					              nav_target_index_ + 1, folder_images_.size());
					text = counter;
				}
				else
				{
					if (folder_images_.size() > 1)
					{
						std::snprintf(counter, sizeof(counter), "%zu / %zu   ",
						              nav_shown_index_ + 1, folder_images_.size());
						text = counter;
					}
					text += readout_dims_;
				}
				if (filter_running_)
					text += text.empty() ? "Dithering\xE2\x80\xA6" : " \xC2\xB7 Dithering\xE2\x80\xA6";

				ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
				if (!last_error_.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
					ImGui::TextUnformatted(text.c_str());
					ImGui::PopStyleColor();
				}
				else
				{
					ImGui::TextUnformatted(text.c_str());
				}
				ImGui::PopFont();
			}
			ImGui::End();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar();
		}

		if (settings_.showControlStrip)
		{
			/* Bottom-centre control strip. */
			ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
			                               vp->WorkPos.y + vp->WorkSize.y - 24.0f),
			                        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 12.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
			if (ImGui::Begin("##controls", nullptr, kChromeFlags))
			{
				ImVec2 r[6][2];
				const bool hasFolder = folder_images_.size() > 1;
				ImGui::BeginDisabled(!hasFolder || nav_shown_index_ == 0);
				if (ImGui::Button("<")) NavigateRelative(-1);
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Fit")) fitToWindow();
				r[0][0] = ImGui::GetItemRectMin(); r[0][1] = ImGui::GetItemRectMax();
				ImGui::SameLine();
				if (ImGui::Button("1:1")) SetZoom1to1();
				r[1][0] = ImGui::GetItemRectMin(); r[1][1] = ImGui::GetItemRectMax();

				ImGui::SameLine(0.0f, 28.0f);
				if (ImGui::Button("-")) adjustZoom(-1);
				r[2][0] = ImGui::GetItemRectMin(); r[2][1] = ImGui::GetItemRectMax();
				ImGui::SameLine();
				char zoomLabel[16];
				std::snprintf(zoomLabel, sizeof(zoomLabel), "%3d%%", getZoomPercent());
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(zoomLabel);
				ImGui::SameLine();
				if (ImGui::Button("+")) adjustZoom(1);
				r[3][0] = ImGui::GetItemRectMin(); r[3][1] = ImGui::GetItemRectMax();

				ImGui::SameLine();
				ImGui::BeginDisabled(!hasFolder || nav_shown_index_ + 1 >= folder_images_.size());
				if (ImGui::Button(">")) NavigateRelative(+1);
				ImGui::EndDisabled();

				ImGui::SameLine(0.0f, 28.0f);
				if (ImGui::Button("Filter"))
					OpenSettings(SettingsTab::Filter, true);
				r[4][0] = ImGui::GetItemRectMin(); r[4][1] = ImGui::GetItemRectMax();

				ImGui::SameLine(0.0f, 28.0f);
				if (ImGui::Button("Settings"))
					OpenSettings(SettingsTab::Interface, true);
				r[5][0] = ImGui::GetItemRectMin(); r[5][1] = ImGui::GetItemRectMax();

				/* One-shot: where the strip buttons landed, so a harness can click
				 * them without a screenshot.  Trace only. */
				if (IviewTraceEnabled() && !stripRectsLogged_ && chromeFrames_ > 20)
				{
					stripRectsLogged_ = true;
					char line[256];
					std::snprintf(line, sizeof(line),
						"rects: fit=%.0f,%.0f one=%.0f,%.0f minus=%.0f,%.0f plus=%.0f,%.0f filter=%.0f,%.0f settings=%.0f,%.0f",
						(r[0][0].x + r[0][1].x) * 0.5f, (r[0][0].y + r[0][1].y) * 0.5f,
						(r[1][0].x + r[1][1].x) * 0.5f, (r[1][0].y + r[1][1].y) * 0.5f,
						(r[2][0].x + r[2][1].x) * 0.5f, (r[2][0].y + r[2][1].y) * 0.5f,
						(r[3][0].x + r[3][1].x) * 0.5f, (r[3][0].y + r[3][1].y) * 0.5f,
						(r[4][0].x + r[4][1].x) * 0.5f, (r[4][0].y + r[4][1].y) * 0.5f,
						(r[5][0].x + r[5][1].x) * 0.5f, (r[5][0].y + r[5][1].y) * 0.5f);
					IviewTrace(line);
					const ImGuiIO& ioNow = ImGui::GetIO();
					int fbw = 0, fbh = 0;
					glfwGetFramebufferSize(glfwWindow, &fbw, &fbh);
					std::snprintf(line, sizeof(line), "viewport: display=%.0fx%.0f fb=%dx%d",
					              ioNow.DisplaySize.x, ioNow.DisplaySize.y, fbw, fbh);
					IviewTrace(line);
				}
			}
			ImGui::End();
			ImGui::PopStyleVar(2);
		}
	}

	/* Open/focus the persistent Settings window. */
	void OpenSettings(SettingsTab tab, bool focus)
	{
		settings_open_ = true;
		settings_tab_request_ = tab;
		settings_tab_request_pending_ = true;
		settings_focus_requested_ = focus;
		IviewTrace("action: settings open");
	}

	void MarkSettingsDirty()
	{
		settings_dirty_ = true;
		settings_status_.clear();
		settings_status_ok_ = true;
	}

	/* Persistent Settings window.  Rendered outside BuildChrome() so chrome
	 * fade and Tab cannot hide it. */
	void BuildSettingsWindow()
	{
		if (!settings_open_) return;

		if (settings_focus_requested_)
		{
			ImGui::SetNextWindowFocus();
			settings_focus_requested_ = false;
		}

		ImGui::SetNextWindowSize(ImVec2(520.0f, 560.0f), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);

		const bool open = ImGui::Begin("Settings", &settings_open_,
		                               ImGuiWindowFlags_NoCollapse);

		if (open)
		{
			const auto tabFlags = [&](SettingsTab tab) -> ImGuiTabItemFlags {
				return (settings_tab_request_pending_ && settings_tab_request_ == tab)
					? ImGuiTabItemFlags_SetSelected : 0;
			};

			if (ImGui::BeginTabBar("SettingsTabs"))
			{
				if (ImGui::BeginTabItem("Interface", nullptr, tabFlags(SettingsTab::Interface)))
				{
					active_settings_tab_ = SettingsTab::Interface;
					BuildSettingsInterfaceTab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Viewing", nullptr, tabFlags(SettingsTab::Viewing)))
				{
					active_settings_tab_ = SettingsTab::Viewing;
					BuildSettingsViewingTab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Filter", nullptr, tabFlags(SettingsTab::Filter)))
				{
					active_settings_tab_ = SettingsTab::Filter;
					BuildSettingsFilterTab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Advanced", nullptr, tabFlags(SettingsTab::Advanced)))
				{
					active_settings_tab_ = SettingsTab::Advanced;
					BuildSettingsAdvancedTab();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			settings_tab_request_pending_ = false;

			ImGui::Separator();
			if (ImGui::Button("Save")) SaveSettings();
			ImGui::SameLine();
			ImGui::BeginDisabled(filter_running_);
			if (ImGui::Button("Reset to defaults"))
				ImGui::OpenPopup("Reset to defaults?");
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Close")) settings_open_ = false;

			/* A failed write stays visible even though dirty remains true. */
			if (!settings_status_.empty() && !settings_status_ok_)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
				                   settings_status_.c_str());
			}
			else if (settings_dirty_)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Unsaved changes");
			}
			else if (!settings_status_.empty())
			{
				ImGui::TextUnformatted(settings_status_.c_str());
			}

			if (ImGui::BeginPopupModal("Reset to defaults?", nullptr,
			                           ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextUnformatted("Reset all settings to builtin defaults?");
				ImGui::TextDisabled("Window size and the config path are preserved.");
				if (ImGui::Button("Reset"))
				{
					ApplyBuiltinDefaults();
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void BuildSettingsInterfaceTab()
	{
		float scale = settings_.uiScale;
		if (ImGui::SliderFloat("UI scale", &scale, 1.0f, 3.0f, "%.1f"))
		{
			settings_.uiScale = std::round(scale * 10.0f) / 10.0f;
			ApplyUiScale(settings_.uiScale);
			MarkSettingsDirty();
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			/* ImGui 1.93 applies FontScaleMain at draw time; no atlas rebuild. */
			IviewTrace("ui_scale committed (no font atlas rebuild needed)");
		}

		if (ImGui::Checkbox("Show readout", &settings_.showReadout)) MarkSettingsDirty();
		if (ImGui::Checkbox("Show control strip", &settings_.showControlStrip)) MarkSettingsDirty();

		int fadeDelay = fadeDelayMs_;
		if (ImGui::SliderInt("Fade delay", &fadeDelay, 200, 5000, "%d ms"))
		{
			fadeDelayMs_ = fadeDelay;
			settings_.fadeDelayMs = fadeDelay;
			MarkSettingsDirty();
		}
		float minAlpha = fadeMinAlpha_;
		if (ImGui::SliderFloat("Min alpha", &minAlpha, 0.0f, 1.0f, "%.2f"))
		{
			fadeMinAlpha_ = minAlpha;
			settings_.fadeMinAlpha = minAlpha;
			MarkSettingsDirty();
		}
		ImGui::TextDisabled("Tab hides the chrome; Settings stays visible.");
	}

	void BuildSettingsViewingTab()
	{
		float bg[3] = { canvasBg_[0], canvasBg_[1], canvasBg_[2] };
		if (ImGui::ColorEdit3("Canvas background", bg, ImGuiColorEditFlags_NoInputs))
		{
			SetCanvasBackground(bg);
			MarkSettingsDirty();
		}

		if (ImGui::Checkbox("Initial zoom: fit", &settings_.initialZoomFit))
			MarkSettingsDirty();
		ImGui::TextDisabled("Unchecked starts at 100%%.");

		int wheel = settings_.wheelAction;
		if (ImGui::RadioButton("Wheel: zoom", &wheel, 0))
		{
			settings_.wheelAction = 0;
			MarkSettingsDirty();
		}
		ImGui::SameLine();
		if (ImGui::RadioButton("Wheel: prev/next", &wheel, 1))
		{
			settings_.wheelAction = 1;
			MarkSettingsDirty();
		}
		if (ImGui::SliderFloat("Wheel nav threshold", &settings_.wheelNavThreshold,
		                       0.1f, 100.0f, "%.1f"))
			MarkSettingsDirty();

		static const char* const kNavZoom[] = { "Preserve", "Fit", "100%" };
		int nav = settings_.navZoomMode;
		if (ImGui::Combo("Navigation zoom", &nav, kNavZoom, IM_ARRAYSIZE(kNavZoom)))
		{
			settings_.navZoomMode = nav;
			MarkSettingsDirty();
		}
	}

	void BuildSettingsFilterTab()
	{
		if (filter_running_)
		{
			ImGui::TextUnformatted("Processing...");
			ImGui::BeginDisabled();
		}

		static const char* const kModes[] = {
			"None", "Ordered (4x4 Bayer)", "Floyd-Steinberg", "PWM (temporal)",
			"Bayer 4x4", "Bayer 8x8", "Blue noise", "Spatial + temporal"
		};
		int mode = static_cast<int>(filterMode_);
		if (ImGui::Combo("Filter mode", &mode, kModes, IM_ARRAYSIZE(kModes)))
		{
			RequestFilterChange(static_cast<uint32_t>(mode));
			MarkSettingsDirty();
		}
		ImGui::TextDisabled("PWM bits: 1 (N-phase output is not implemented)");

		ImGui::Separator();
		/* Backend choice.  Switching to GPU needs one re-upload: the tiles
		 * currently hold finished RGBA8 frames rather than the source, so the
		 * picture has to be published again at native precision. */
		int backend = (filterBackend_ == Config::FilterBackend::Gpu) ? 1 : 0;
		if (ImGui::Combo("Compute", &backend, "CPU (plugin)\0GPU (shader)\0"))
		{
			SetFilterBackend(backend == 1 ? Config::FilterBackend::Gpu : Config::FilterBackend::Cpu);
			MarkSettingsDirty();
		}
		if (filterBackend_ == Config::FilterBackend::Gpu && !ditherReady_)
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
			                   "The dither shader did not compile; using CPU.");
		else if (filterBackend_ == Config::FilterBackend::Gpu && !ModeSupportedOnGpu(filterMode_))
			ImGui::TextDisabled("Floyd-Steinberg always runs on the CPU.");
		else if (filterBackend_ == Config::FilterBackend::Gpu)
			ImGui::TextDisabled("Dithering happens while drawing; mode changes are free.");
		else
			ImGui::TextDisabled("The plugin quantises once per image, on a worker thread.");

		if (filter_running_) ImGui::EndDisabled();

		if (filter_failed_ && !filter_error_.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
			ImGui::TextWrapped("filter failed: %s", filter_error_.c_str());
			ImGui::PopStyleColor();
		}
		else if (!source_image_)
		{
			ImGui::TextDisabled("The selected mode is applied when an image loads.");
		}
		else if (filter_running_)
		{
			ImGui::TextDisabled("Recomputing...");
		}
	}

	void BuildSettingsAdvancedTab()
	{
		ImGui::TextUnformatted("Loaded from");
		ImGui::TextWrapped("%s", settings_.loadPath.empty()
			? "(builtin defaults)" : settings_.loadPath.c_str());
		ImGui::TextUnformatted("Save target");
		ImGui::TextWrapped("%s", settings_.savePath.empty()
			? "(none)" : settings_.savePath.c_str());
		ImGui::Separator();
		if (ImGui::Checkbox("Remember window position and size", &settings_.rememberWindowSize))
			MarkSettingsDirty();
		ImGui::TextDisabled("Position, size and maximised state are captured when the window closes.");
	}

	/* The clear colour is consumed every frame by DrawImage, so this only has to
	 * update the members — no re-upload, no render-path change. */
	void SetCanvasBackground(const float rgb[3])
	{
		for (int i = 0; i < 3; ++i)
			canvasBg_[i] = std::clamp(rgb[i], 0.0f, 1.0f);
		for (int i = 0; i < 3; ++i)
			settings_.canvasBg[i] = static_cast<int>(std::lround(canvasBg_[i] * 255.0f));
	}

	/* Reset all persisted settings except the window placement and the config
	 * paths. */
	void ApplyBuiltinDefaults()
	{
		const Config::Settings defaults{};
		const std::string loadPath = settings_.loadPath;
		const std::string savePath = settings_.savePath;
		const int windowWidth = settings_.windowWidth;
		const int windowHeight = settings_.windowHeight;
		const int windowX = settings_.windowX;
		const int windowY = settings_.windowY;
		const bool windowMaximized = settings_.windowMaximized;

		settings_ = defaults;
		settings_.loadPath = loadPath;
		settings_.savePath = savePath;
		settings_.windowWidth = windowWidth;
		settings_.windowHeight = windowHeight;
		settings_.windowX = windowX;
		settings_.windowY = windowY;
		settings_.windowMaximized = windowMaximized;

		fadeDelayMs_  = settings_.fadeDelayMs;
		fadeMinAlpha_ = settings_.fadeMinAlpha;
		for (int i = 0; i < 3; ++i)
			canvasBg_[i] = settings_.canvasBg[i] / 255.0f;
		pwmBits_ = settings_.pwmBits;
		if (imgui_) imgui_->chromeEnabled = settings_.chromeVisible;
		ApplyUiScale(settings_.uiScale);

		filter_failed_ = false;
		filter_error_.clear();
		filterBackend_ = settings_.filterBackend;
		if (!filter_running_)
			RequestFilterChange(settings_.filterMode);
		else
			settings_.filterMode = filterMode_;

		MarkSettingsDirty();
	}

	void SyncSettingsFromLiveState()
	{
		settings_.fadeDelayMs  = fadeDelayMs_;
		settings_.fadeMinAlpha = fadeMinAlpha_;
		settings_.filterMode   = filterMode_;
		settings_.filterBackend = filterBackend_;
		settings_.pwmBits      = pwmBits_;
		if (imgui_) settings_.chromeVisible = imgui_->chromeEnabled;
		for (int i = 0; i < 3; ++i)
			settings_.canvasBg[i] = static_cast<int>(std::lround(canvasBg_[i] * 255.0f));
	}

	bool SaveSettings()
	{
		SyncSettingsFromLiveState();
		/* An explicit Save should persist the placement too, not only the
		 * controls the panel shows. */
		CaptureWindowGeometry();
		const bool ok = Config::Save(settings_);
		settings_status_ok_ = ok;
		if (ok)
		{
			settings_dirty_ = false;
			settings_status_ = settings_.savePath.empty()
				? std::string("Saved (no config path)")
				: ("Saved to " + settings_.savePath);
		}
		else
		{
			settings_status_ = settings_.savePath.empty()
				? std::string("Write failed: no config path")
				: ("Write failed: " + settings_.savePath);
		}
		return ok;
	}

	/* File drop: phase 1 reuses the same decode + upload path on the main
	 * thread (this runs inside glfwPollEvents, between two frames). */
	void HandleDrop(int count, const char** paths)
	{
		if (count <= 0 || paths == nullptr || !imgui_) return;
		/* One plugin call at a time: a drop while a re-run is computing would
		 * enter the plugin from two threads.  Dropped files are simply ignored
		 * for the ~100 ms a compute takes. */
		if (filter_running_) return;
		if (IviewTraceEnabled())
		{
			char line[128];
			std::snprintf(line, sizeof(line), "drop start count=%d", count);
			IviewTrace(line);
		}

		for (int i = 0; i < count; ++i)
		{
			if (paths[i] == nullptr) continue;
			try
			{
				const std::filesystem::path path(reinterpret_cast<const char8_t*>(paths[i]));
				/* ACDSee behaviour: the dropped file's folder becomes the current
				 * folder and the dropped file becomes the current index. */
				ScanFolder(path);
				if (IviewTraceEnabled())
				{
					char line[192];
					std::snprintf(line, sizeof(line),
					              "drop clearing nav_in_flight (was=%d) target=%zu",
					              nav_in_flight_ ? 1 : 0, folder_index_);
					IviewTrace(line);
				}
				if (nav_future_.valid())
				{
					IviewTrace("drop draining nav future");
					try { nav_future_.wait(); (void)nav_future_.get(); }
					catch (...) { IviewTrace("drop nav future drain caught exception"); }
				}
				nav_shown_index_  = folder_index_;
				nav_target_index_ = folder_index_;
				nav_in_flight_    = false;

				auto image = std::make_shared<Image>(Image::FromFile(path));
				IWFilterArg noneArgs[1];
				const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
				if (!LoadTextures(*image, noneOpts))
					continue;
				SetWindowTitle(path);
				ComputeReadout(*image, path);
				if (IviewTraceEnabled())
				{
					char line[192];
					std::snprintf(line, sizeof(line),
					              "drop replacing source old=%p gen=%llu",
					              static_cast<const void*>(source_image_.get()),
					              static_cast<unsigned long long>(file_generation_));
					IviewTrace(line);
				}
				source_image_ = image;
				current_path_ = path;
				++file_generation_;
				last_error_.clear();
				MaybeStartDeferredFilter();
				IviewTrace("drop done");
				if (!imgui_->chromeEnabled)
				{
					imgui_->chromeEnabled = true;
					MarkSettingsDirty();
				}
				imgui_->lastMouseActivity = std::chrono::steady_clock::now();
				return;
			}
			catch (const std::exception& ex)
			{
				if (IviewTraceEnabled())
				{
					char line[512];
					std::snprintf(line, sizeof(line), "drop failed: %s", ex.what());
					IviewTrace(line);
				}
				/* Try the next dropped file. */
			}
		}
	}

public:
	void setZoom(double zoomLevel) {
		setZoom(zoomLevel, width * 0.5, height * 0.5);
	}

	void setZoom(double zoomLevel, double cursorX, double cursorY) {
		IviewTrace("action: set_zoom");
		zoom_mode_ = ZoomMode::Manual;
		ZoomAround(zoomLevel, cursorX, cursorY);
	}

	/* `steps` is zoom-scale steps (see ZoomScale.hpp), not percentage points:
	 * positive zooms in.  Anchored on the window centre. */
	void adjustZoom(int steps) {
		IviewTrace("action: adjust_zoom");
		ZoomAround(ZoomScale::Step(getZoom(), steps), width * 0.5, height * 0.5);
	}

	void adjustPan(int dx, int dy) {
		setPanning(panX + dx, panY + dy);
	}

	void fitToWindow() {
		IviewTrace("action: fit");
		setPanning(0.0, 0.0);
		if (HasImage()) zoom_ = ZoomScale::Fit(imageWidth_, imageHeight_, width, height);
	}

	static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
		Window* containerWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (containerWindow) {
			/* Let ImGui take the keyboard while one of its widgets has focus. */
			if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard) return;

			/* Folder navigation.  Clamped at the ends by NavigateRelative. */
			if (action == GLFW_PRESS || action == GLFW_REPEAT) {
				switch (key) {
				case GLFW_KEY_RIGHT:
				case GLFW_KEY_SPACE:
				case GLFW_KEY_PAGE_DOWN:
					containerWindow->NavigateRelative(+1);
					break;
				case GLFW_KEY_LEFT:
				case GLFW_KEY_BACKSPACE:
				case GLFW_KEY_PAGE_UP:
					containerWindow->NavigateRelative(-1);
					break;
				case GLFW_KEY_HOME:
					containerWindow->NavigateTo(0);
					break;
				case GLFW_KEY_END:
					if (!containerWindow->folder_images_.empty())
						containerWindow->NavigateTo(containerWindow->folder_images_.size() - 1);
					break;
				case GLFW_KEY_F:
					containerWindow->fitToWindow();
					containerWindow->zoom_mode_ = Window::ZoomMode::Fit;
					break;
				case GLFW_KEY_1:
					containerWindow->SetZoom1to1();
					break;
				default:
					break;
				}
			}
			/* Tab toggles the chrome.  That this callback still runs is also the
			 * proof that ImGui_ImplGlfw_InitForOpenGL(..., true) chained the
			 * previously installed GLFW callbacks rather than replacing them. */
			if (key == GLFW_KEY_TAB && action == GLFW_PRESS && containerWindow->imgui_) {
				containerWindow->imgui_->chromeEnabled = !containerWindow->imgui_->chromeEnabled;
				containerWindow->imgui_->lastMouseActivity = std::chrono::steady_clock::now();
				containerWindow->MarkSettingsDirty();
				IviewTrace(containerWindow->imgui_->chromeEnabled ? "chrome shown (Tab)"
				                                                  : "chrome hidden (Tab)");
			}
			containerWindow->hotkeysHandler.key_callback(window, key, scancode, action, mods);
		}
	}
};
