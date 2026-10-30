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
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "FilterPluginDef.h"
#include "FilterCore.hpp"
#include "Config.hpp"

export module UserInterface;

import Image;
import HotkeysHandler;
import FilterPlugin;

/* ── Startup trace ────────────────────────────────────────────────────────────
 *
 * Milliseconds since the first trace call in the process.  Enabled only when
 * IMAGEVIEWER_VK_TRACE is set to something other than "0" (the name predates the
 * OpenGL renderer; it is kept so existing probe scripts keep working).
 * Exported because main_generic.hpp marks "async decode launched". */
static const std::chrono::steady_clock::time_point& IviewTraceStart() noexcept
{
	static const std::chrono::steady_clock::time_point t0 =
		std::chrono::steady_clock::now();
	return t0;
}

export bool IviewTraceEnabled() noexcept
{
	static const bool enabled = [] {
		const char* v = std::getenv("IMAGEVIEWER_VK_TRACE");
		return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
	}();
	return enabled;
}

export void IviewTrace(const char* what) noexcept
{
	if (!IviewTraceEnabled()) return;
	const auto t0 = IviewTraceStart();
	const double ms = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	std::fprintf(stderr, "IVIEW_TRACE +%8.2f ms  %s\n", ms, what);
	std::fflush(stderr);
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
			/* The viewport follows the framebuffer, which is what the quad's
			 * projection is computed against. */
			win->width  = width;
			win->height = height;
			glViewport(0, 0, width, height);
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
		constexpr int MIN_ZOOM_PERCENT = -90;
		constexpr int MAX_ZOOM_PERCENT = 400;
		constexpr double SCROLL_SENSITIVITY = 10;

		double mouseX, mouseY;
		glfwGetCursorPos(glfwWindow, &mouseX, &mouseY);

		double newZoomLevel = (scroll + yoffset * SCROLL_SENSITIVITY + 100.0) / 100.0;
		newZoomLevel = std::clamp(newZoomLevel,
			(MIN_ZOOM_PERCENT + 100.0) / 100.0, (MAX_ZOOM_PERCENT + 100.0) / 100.0);

		setZoom(newZoomLevel, mouseX, mouseY);
	}

private:
	GLFWwindow* glfwWindow = nullptr;
	int width;
	int height;
	int scroll;
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
	 * RGB8 — which is the accepted price of live filter switching. */
	std::unique_ptr<Image> source_image_;

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
	bool     filter_panel_open_ = false;
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
	bool     panelRectsLogged_  = false;
	int      chromeFrames_      = 0;

	/* ── GL resources ──────────────────────────────────────────────────────── */
	struct Texture
	{
		GLuint id = 0;
		int    width = 0;
		int    height = 0;
	};
	Texture textures_[2];      /* phase A / phase B (identical when single-frame) */
	int     textureCount_ = 0;

	GLuint shaderProgram_  = 0;
	GLuint vao_            = 0;
	GLuint vboPositions_   = 0;
	GLuint vboTexCoords_   = 0;
	GLint  uniformProjection_ = -1;
	GLint  uniformModel_      = -1;
	GLint  uniformTexture_    = -1;

	double getZoom() const {
		return (scroll + 100.0) / 100.0;
	}

	int getZoomPercent() const noexcept {
		return static_cast<int>(std::lround(getZoom() * 100.0));
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
		glfwTerminate();
	}

private:
	HotkeysHandler hotkeysHandler;

public:
	Window(int width, int height, const Config::Settings& settings)
		: glfwWindow(InitializeWindow(width, height, this)),
		  width(width),
		  height(height),
		  scroll(0),
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

		glEnable(GL_TEXTURE_2D);

		CreateQuadResources();
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

	~Window()
	{
		if (glfwWindow == nullptr) return;

		ShutdownImGui();

		for (Texture& t : textures_)
		{
			if (t.id != 0) { glDeleteTextures(1, &t.id); t.id = 0; }
		}
		if (vboTexCoords_ != 0) glDeleteBuffers(1, &vboTexCoords_);
		if (vboPositions_ != 0) glDeleteBuffers(1, &vboPositions_);
		if (vao_ != 0)          glDeleteVertexArrays(1, &vao_);
		if (shaderProgram_ != 0) glDeleteProgram(shaderProgram_);

		glfwDestroyWindow(glfwWindow);
		glfwWindow = nullptr;
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
			void main()
			{
				gl_Position = projection * model * vec4(aPos.x, aPos.y, 0.0, 1.0);
				TexCoord = aTexCoord;
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

		/* Unit quad, triangle strip.  Texture coordinate v = 0 is the first row
		 * of the uploaded image, which DrawImage() places at the top of the
		 * screen by using a negative y scale. */
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

		/* All controls at 2x: the strip, panel and readout were too small at
		 * 100 %.  ScaleAllSizes covers paddings, spacing, rounding and the
		 * widget geometry; FontSizeBase scales the text with them. */
		style.ScaleAllSizes(2.0f);
		style.FontScaleMain = 2.0f;   /* text; GetFontSize() = FontSizeBase * this */

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
		return pd;
	}

	/* Exactly the plugin's target format: interleaved 8-bit RGBA, in order. */
	static bool IsPackedRgba8(const IWImageFormat& f) noexcept
	{
		if (f.storageLayout != IW_STORAGE_INTERLEAVED) return false;
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

	/* Convert one frame to tightly packed RGBA8.  The plugin already produces
	 * exactly that layout, so the common path is a straight row copy; anything
	 * else (float/16-bit/exotic semantics when no plugin is loaded) goes through
	 * the same FilterCore helper the plugin uses. */
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

	/* One texture per phase frame.  A single-frame source uploads the same
	 * pixels twice, so the alternation stays harmless. */
	void UploadTextures(const std::vector<uint8_t>& phaseA,
	                    const std::vector<uint8_t>& phaseB,
	                    int width, int height)
	{
		const uint8_t* sources[2] = {
			phaseA.data(),
			phaseB.empty() ? phaseA.data() : phaseB.data()
		};

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		for (int i = 0; i < 2; ++i)
		{
			if (textures_[i].id == 0)
				glGenTextures(1, &textures_[i].id);

			glBindTexture(GL_TEXTURE_2D, textures_[i].id);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, sources[i]);

			textures_[i].width  = width;
			textures_[i].height = height;
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		textureCount_ = phaseB.empty() ? 1 : 2;
	}

	/* ── Filter pipeline: compute (any thread) / upload (main thread) ──────── */

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

		if (filterPlugin_.IsLoaded()) {
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
		const uint32_t count = std::min<uint32_t>(image.format.componentCount, IW_MAX_COMPONENTS);
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
		const Image* const src = source_image_.get();
		const IWFilterOptions opts = filterOptions_;     /* argv -> filterArgStorage_ */
		filter_future_ = std::async(std::launch::async, [this, src, opts] {
			return ComputeFilteredFrames(*src, opts);
		});
		IviewTrace("filter compute started");
	}

	/* Called once an image is on screen in mode=none: queue the requested mode in
	 * the background so the window never waits on it. */
	void MaybeStartDeferredFilter()
	{
		if (!source_image_ || filter_running_) return;
		if (filterMode_ == 0u) return;                   /* nothing requested */
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
		if (!source_image_ || filter_running_) return;   /* selector is disabled */
		if (mode == filterMode_) return;

		filterMode_    = mode;      /* the radio reflects the click immediately */
		filter_failed_ = false;
		filter_error_.clear();
		RebuildFilterOptions();
		StartFilterCompute();
	}

	/* ── Folder navigation ─────────────────────────────────────────────────── */

	static bool IsSupportedImage(const std::filesystem::path& p)
	{
		static const char* const kExtensions[] = {
			".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".apng", ".bmp", ".dib",
			".tif", ".tiff", ".webp", ".gif", ".avif", ".heic", ".heif", ".hdr",
			".exr", ".pic", ".ppm", ".pgm", ".pbm", ".pnm", ".tga"
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
		if (!nav_in_flight_) StartNavDecode();
	}

	void StartNavDecode()
	{
		nav_in_flight_ = true;
		const size_t idx = nav_target_index_;
		const std::filesystem::path p = folder_images_[idx];
		/* TODO(prefetch): decode idx+1 while the user looks at idx. */
		IviewTrace("nav decode started");
		nav_future_ = std::async(std::launch::async, [idx, p] {
			return DecodeResult{ idx, Image::FromFile(p) };
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
	 * OpenGL name so the call sites do not change. */
	bool LoadTextures(
		const Image& image,
		const int /*segmentWidth*/,
		const int /*segmentHeight*/,
		const int /*redundantBorderSize*/,
		const IWFilterOptions& opts = IWFilterOptions{
			IW_FILTER_OPTIONS_VERSION, 0u, nullptr })
	{
		try
		{
			UploadFrameSet(ComputeFilteredFrames(image, opts));
			appliedMode_ = 0;          /* LoadTextures always uploads mode=none */
			CenterImage();
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
		if (!settings_.initialZoomFit) { scroll = 0; return; }   /* 100 % */
		const double minRatio = std::min<double>((double)width / imageWidth_,
		                                         (double)height / imageHeight_);
		this->scroll = static_cast<int>(minRatio * 100.0 - 100);
	}

	/* ── Main loop ─────────────────────────────────────────────────────────── */

	/* Draws the image quad for one phase.  The transform is built in NDC on the
	 * CPU, so the shader keeps the same projection*model pipeline as the OpenGL
	 * branch while needing no per-frame buffer updates. */
	void DrawImage(uint32_t phase) const
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, width, height);
		glClearColor(canvasBg_[0], canvasBg_[1], canvasBg_[2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		if (!HasImage()) return;

		const Texture& tex = textures_[phase & 1u];
		if (tex.id == 0) return;

		/* At 100 % or more show exact pixels, below that filter them. */
		const double zoom = getZoom();
		glBindTexture(GL_TEXTURE_2D, tex.id);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
		                (zoom >= 1.0) ? GL_NEAREST : GL_LINEAR);

		const double imgW = imageWidth_ * zoom;
		const double imgH = imageHeight_ * zoom;
		const double imgX = (width - imgW) * 0.5 + panX;
		const double imgY = (height - imgH) * 0.5 + panY;

		/* Unit quad -> NDC.  y is negated so image row 0 lands at the top. */
		const float sx = (float)(2.0 * imgW / width);
		const float sy = (float)(-2.0 * imgH / height);
		const float tx = (float)(2.0 * imgX / width - 1.0);
		const float ty = (float)(1.0 - 2.0 * imgY / height);

		/* Column-major model matrix; the projection stays identity. */
		const float model[16] = {
			sx,   0.0f, 0.0f, 0.0f,
			0.0f, sy,   0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			tx,   ty,   0.0f, 1.0f
		};
		const float identity[16] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};

		glUseProgram(shaderProgram_);
		glUniformMatrix4fv(uniformProjection_, 1, GL_FALSE, identity);
		glUniformMatrix4fv(uniformModel_, 1, GL_FALSE, model);
		glUniform1i(uniformTexture_, 0);

		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
	}

	void Main(std::future<Image>&& imageFuture, std::filesystem::path imagePath)
	{
		using Clock = std::chrono::steady_clock;

		bool uploaded = false;
		bool shown    = false;
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

			if (firstLoop) { IviewTrace("first loop iteration"); firstLoop = false; }

			/* (1) Decode future.  Nothing blocks here: until it is ready the
			 * loop just presents clear frames (the window is still hidden). */
			if (!uploaded &&
			    imageFuture.valid() &&
			    imageFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				IviewTrace("future ready");
				try
				{
					/* Retained for the lifetime of this file so the filter can be
					 * re-run without decoding again (see source_image_). */
					auto image = std::make_unique<Image>(imageFuture.get());

					/* The first visible frame is always the plain 8-bit image.
					 * A requested PWM mode is computed in the background and
					 * swapped in by the block below once it is ready, so a
					 * high-bit-depth source never delays start-up. */
					IWFilterArg noneArgs[1];
					const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
					if (!LoadTextures(*image, 0, 0, 0, noneOpts))
						throw std::runtime_error("the filter produced an unsupported frame count");
					SetWindowTitle(imagePath);
					ComputeReadout(*image, imagePath);
					source_image_ = std::move(image);
					current_path_ = imagePath;
					++file_generation_;

					/* Folder context.  A directory argument arrives here as its
					 * first image, so the parent is always the right folder. */
					try {
						ScanFolder(imagePath);
						nav_shown_index_  = folder_index_;
						nav_target_index_ = folder_index_;
					} catch (const std::exception& ex) {
						folder_images_.clear();       /* stay single-file */
						std::fprintf(stderr, "iview: %s\n", ex.what());
					}
					uploaded = true;
					IviewTrace("image uploaded to the GPU");
					/* Kicked after the window is on screen (see the show block
					 * below): creating the worker before the first swap would
					 * put thread start-up between the upload and the show. */
					pendingDeferredFilter_ = true;
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

			/* (1a) Navigation decode finished.  Everything happens on this thread;
			 * the old image stayed on screen while the worker decoded. */
			if (nav_in_flight_ && nav_future_.valid() &&
			    nav_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				nav_in_flight_ = false;
				try
				{
					DecodeResult r = nav_future_.get();
					if (r.index == nav_target_index_)
					{
						IWFilterArg noneArgs[1];
						const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
						if (!LoadTextures(r.image, 0, 0, 0, noneOpts))
							throw std::runtime_error("the filter produced an unsupported frame count");

						const std::filesystem::path& shownPath = folder_images_[r.index];
						SetWindowTitle(shownPath);
						ComputeReadout(r.image, shownPath);
						source_image_ = std::make_unique<Image>(std::move(r.image));
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
				const double computeMs = std::chrono::duration<double, std::milli>(
					Clock::now() - filterStartTime_).count();
				try
				{
					FilterFrameSet fs = filter_future_.get();
					if (filterGeneration_ != file_generation_)
					{
						/* The user navigated while this was computing: the result
						 * belongs to a file that is no longer on screen. */
						IviewTrace("stale filter result discarded");
					}
					else
					{
						const auto uploadStart = Clock::now();
						UploadFrameSet(fs);
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

			/* (2) The whole visible path: no dirty flag, no FBO, redraw every
			 * frame from whichever phase texture is current. */
			if (uploaded)
			{
				DrawImage(phase);
				if (framesAfterUpload == 0) IviewTrace("first frame drawn");

				ImGui_ImplOpenGL3_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();
				BuildChrome();
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

			if (uploaded)
			{
				if (framesAfterUpload == 0) IviewTrace("first swap");

				/* First ten frames: prove the A/B alternation (0,1,0,1,…). */
				if (framesAfterUpload < 10)
				{
					std::fprintf(stderr, "IVIEW_FRAME %d phase=%u texture=%u\n",
					             framesAfterUpload, phase & 1u, textures_[phase & 1u].id);
					std::fflush(stderr);
				}
				++framesAfterUpload;
				phase ^= 1u;
			}

			/* (4) The image is already in the frame that was just swapped in;
			 * only now does the window become visible. */
			if (uploaded && !shown)
			{
				glfwShowWindow(glfwWindow);
				shown = true;
				IviewTrace("glfwShowWindow");
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

		/* Persist the panel's state (spec: save on close). */
		SaveSettings();
		ShutdownImGui();
	}

private:
	void SetWindowTitle(const std::filesystem::path& path)
	{
		const std::string title = "ImageViewer \xE2\x80\x94 " + Utf8Filename(path);
		glfwSetWindowTitle(glfwWindow, title.c_str());
	}

	void ComputeReadout(const Image& image, const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
		if (ec)
		{
			readout_dims_ = std::format("{}\xC3\x97{}", image.width, image.height);
		}
		else
		{
			const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
			readout_dims_ = std::format("{}\xC3\x97{} \xC2\xB7 {:.1f} MB",
			                              image.width, image.height, megabytes);
		}
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

		/* Bottom-centre control strip. */
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
		                               vp->WorkPos.y + vp->WorkSize.y - 24.0f),
		                        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 12.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
		if (ImGui::Begin("##controls", nullptr, kChromeFlags))
		{
			ImVec2 r[5][2];
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
			if (ImGui::Button("-")) adjustZoom(-10);
			r[2][0] = ImGui::GetItemRectMin(); r[2][1] = ImGui::GetItemRectMax();
			ImGui::SameLine();
			char zoomLabel[16];
			std::snprintf(zoomLabel, sizeof(zoomLabel), "%3d%%", getZoomPercent());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(zoomLabel);
			ImGui::SameLine();
			if (ImGui::Button("+")) adjustZoom(10);
			r[3][0] = ImGui::GetItemRectMin(); r[3][1] = ImGui::GetItemRectMax();

			ImGui::SameLine();
			ImGui::BeginDisabled(!hasFolder || nav_shown_index_ + 1 >= folder_images_.size());
			if (ImGui::Button(">")) NavigateRelative(+1);
			ImGui::EndDisabled();

			ImGui::SameLine(0.0f, 28.0f);
			if (ImGui::Button("Filter"))
			{
				filter_panel_open_ = !filter_panel_open_;
				IviewTrace(filter_panel_open_ ? "action: filter panel open"
				                              : "action: filter panel closed");
			}
			r[4][0] = ImGui::GetItemRectMin(); r[4][1] = ImGui::GetItemRectMax();

			/* One-shot: where the strip buttons landed, so a harness can click
			 * them without a screenshot.  Trace only. */
			if (IviewTraceEnabled() && !stripRectsLogged_ && chromeFrames_ > 20)
			{
				stripRectsLogged_ = true;
				char line[256];
				std::snprintf(line, sizeof(line),
					"rects: fit=%.0f,%.0f one=%.0f,%.0f minus=%.0f,%.0f plus=%.0f,%.0f filter=%.0f,%.0f",
					(r[0][0].x + r[0][1].x) * 0.5f, (r[0][0].y + r[0][1].y) * 0.5f,
					(r[1][0].x + r[1][1].x) * 0.5f, (r[1][0].y + r[1][1].y) * 0.5f,
					(r[2][0].x + r[2][1].x) * 0.5f, (r[2][0].y + r[2][1].y) * 0.5f,
					(r[3][0].x + r[3][1].x) * 0.5f, (r[3][0].y + r[3][1].y) * 0.5f,
					(r[4][0].x + r[4][1].x) * 0.5f, (r[4][0].y + r[4][1].y) * 0.5f);
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

		if (filter_panel_open_) BuildOptionsPanel();
	}

	/* Filter + viewing settings, opened by the strip's Filter button. */
	void BuildOptionsPanel()
	{
		if (!ImGui::Begin("Options", &filter_panel_open_,
		                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		/* ── Filter ─────────────────────────────────────────────────────────── */
		ImGui::TextUnformatted("Filter");
		ImGui::Separator();

		if (filter_running_)
		{
			ImGui::TextUnformatted("processing...");
			ImGui::BeginDisabled();
		}
		else if (!source_image_)
		{
			ImGui::BeginDisabled();          /* nothing to re-filter yet */
		}

		int mode = static_cast<int>(filterMode_);
		if (ImGui::RadioButton("None", &mode, 0))           RequestFilterChange(0u);
		ImVec2 noneMin = ImGui::GetItemRectMin(), noneMax = ImGui::GetItemRectMax();
		if (ImGui::RadioButton("PWM (temporal)", &mode, 3)) RequestFilterChange(3u);
		ImVec2 pwmMin = ImGui::GetItemRectMin(), pwmMax = ImGui::GetItemRectMax();
		/* One-shot rect log for the automated check (trace only). */
		if (IviewTraceEnabled() && !panelRectsLogged_ && chromeFrames_ > 20)
		{
			panelRectsLogged_ = true;
			char line[128];
			std::snprintf(line, sizeof(line), "panel rects: none=%.0f,%.0f pwm=%.0f,%.0f",
			              (noneMin.x + noneMax.x) * 0.5f, (noneMin.y + noneMax.y) * 0.5f,
			              (pwmMin.x + pwmMax.x) * 0.5f, (pwmMin.y + pwmMax.y) * 0.5f);
			IviewTrace(line);
		}
		/* TODO(side-by-side): original next to the filtered result — deferred. */

		if (filter_running_ || !source_image_) ImGui::EndDisabled();

		if (filter_failed_ && !filter_error_.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
			ImGui::TextWrapped("filter failed: %s", filter_error_.c_str());
			ImGui::PopStyleColor();
		}
		else if (!source_image_)
		{
			ImGui::TextDisabled("no image loaded yet");
		}

		ImGui::Separator();
		/* ── Viewing settings ───────────────────────────────────────────────── */
		ImGui::TextUnformatted("Viewing");

		float bg[3] = { canvasBg_[0], canvasBg_[1], canvasBg_[2] };
		if (ImGui::ColorEdit3("Canvas background", bg, ImGuiColorEditFlags_NoInputs))
			SetCanvasBackground(bg);

		ImGui::SliderInt("Fade delay", &fadeDelayMs_, 200, 5000, "%d ms");
		ImGui::SliderFloat("Min alpha", &fadeMinAlpha_, 0.0f, 1.0f, "%.2f");

		int wheel = settings_.wheelAction;
		if (ImGui::RadioButton("Wheel: zoom", &wheel, 0)) settings_.wheelAction = 0;
		ImGui::SameLine();
		if (ImGui::RadioButton("Wheel: prev/next", &wheel, 1)) settings_.wheelAction = 1;

		ImGui::Separator();
		if (ImGui::Button("Reset to defaults")) LoadBuiltinDefaults();
		ImGui::SameLine();
		if (ImGui::Button("Save")) SaveSettings();

		ImGui::End();
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

	/* Panel-only reset: the filter mode and the live viewing knobs.  Window size
	 * is start-up state and is left alone. */
	void LoadBuiltinDefaults()
	{
		const Config::Settings defaults{};
		fadeDelayMs_  = defaults.fadeDelayMs;
		fadeMinAlpha_ = defaults.fadeMinAlpha;
		for (int i = 0; i < 3; ++i)
		{
			canvasBg_[i]          = defaults.canvasBg[i] / 255.0f;
			settings_.canvasBg[i] = defaults.canvasBg[i];
		}
		settings_.fadeDelayMs  = fadeDelayMs_;
		settings_.fadeMinAlpha = fadeMinAlpha_;
		settings_.initialZoomFit = defaults.initialZoomFit;
		if (filterMode_ != defaults.filterMode) RequestFilterChange(defaults.filterMode);
	}

	void SaveSettings()
	{
		settings_.fadeDelayMs  = fadeDelayMs_;
		settings_.fadeMinAlpha = fadeMinAlpha_;
		settings_.filterMode   = filterMode_;
		settings_.pwmBits      = pwmBits_;
		if (imgui_) settings_.chromeVisible = imgui_->chromeEnabled;
		for (int i = 0; i < 3; ++i)
			settings_.canvasBg[i] = static_cast<int>(std::lround(canvasBg_[i] * 255.0f));
		Config::Save(settings_);
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

		for (int i = 0; i < count; ++i)
		{
			if (paths[i] == nullptr) continue;
			try
			{
				const std::filesystem::path path(reinterpret_cast<const char8_t*>(paths[i]));
				/* ACDSee behaviour: the dropped file's folder becomes the current
				 * folder and the dropped file becomes the current index. */
				ScanFolder(path);
				nav_shown_index_  = folder_index_;
				nav_target_index_ = folder_index_;
				nav_in_flight_    = false;

				auto image = std::make_unique<Image>(Image::FromFile(path));
				IWFilterArg noneArgs[1];
				const IWFilterOptions noneOpts = ModeOnlyOptions(0u, noneArgs);
				if (!LoadTextures(*image, 0, 0, 0, noneOpts))
					continue;
				SetWindowTitle(path);
				ComputeReadout(*image, path);
				source_image_ = std::move(image);
				current_path_ = path;
				++file_generation_;
				last_error_.clear();
				MaybeStartDeferredFilter();
				imgui_->chromeEnabled     = true;
				imgui_->lastMouseActivity = std::chrono::steady_clock::now();
				return;
			}
			catch (const std::exception&)
			{
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
		const double currentZoom = getZoom();
		const double newScroll = (zoomLevel * 100.0) - 100.0;
		scroll = std::clamp(static_cast<int>(newScroll), -90, 400);

		if (HasImage()) {
			const double imgW = imageWidth_ * currentZoom;
			const double imgH = imageHeight_ * currentZoom;
			const double imgX = (width - imgW) * 0.5 + panX;
			const double imgY = (height - imgH) * 0.5 + panY;

			const double mouseImgXBefore = (cursorX - imgX) / currentZoom;
			const double mouseImgYBefore = (cursorY - imgY) / currentZoom;

			const double newZoom = getZoom();
			const double newImgW = imageWidth_ * newZoom;
			const double newImgH = imageHeight_ * newZoom;
			const double newImgX = (width - newImgW) * 0.5 + panX;
			const double newImgY = (height - newImgH) * 0.5 + panY;
			const double mouseScreenXAfter = newImgX + mouseImgXBefore * newZoom;
			const double mouseScreenYAfter = newImgY + mouseImgYBefore * newZoom;

			setPanning(panX + cursorX - mouseScreenXAfter, panY + cursorY - mouseScreenYAfter);
		}
	}

	void adjustZoom(int zoomDelta) {
		IviewTrace("action: adjust_zoom");
		const double currentZoom = getZoom();
		scroll = std::clamp(scroll + zoomDelta, -90, 400);

		if (HasImage()) {
			const double imgW = imageWidth_ * currentZoom;
			const double imgH = imageHeight_ * currentZoom;
			const double imgX = (width - imgW) * 0.5 + panX;
			const double imgY = (height - imgH) * 0.5 + panY;

			const double centerX = width * 0.5;
			const double centerY = height * 0.5;
			const double mouseImgXBefore = (centerX - imgX) / currentZoom;
			const double mouseImgYBefore = (centerY - imgY) / currentZoom;

			const double newZoom = getZoom();
			const double newImgW = imageWidth_ * newZoom;
			const double newImgH = imageHeight_ * newZoom;
			const double newImgX = (width - newImgW) * 0.5 + panX;
			const double newImgY = (height - newImgH) * 0.5 + panY;
			const double mouseScreenXAfter = newImgX + mouseImgXBefore * newZoom;
			const double mouseScreenYAfter = newImgY + mouseImgYBefore * newZoom;

			setPanning(panX + centerX - mouseScreenXAfter, panY + centerY - mouseScreenYAfter);
		}
	}

	void adjustPan(int dx, int dy) {
		setPanning(panX + dx, panY + dy);
	}

	void fitToWindow() {
		IviewTrace("action: fit");
		setPanning(0.0, 0.0);
		if (HasImage()) {
			const double minRatio = std::min(static_cast<double>(width) / imageWidth_,
											 static_cast<double>(height) / imageHeight_);
			scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
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
				IviewTrace(containerWindow->imgui_->chromeEnabled ? "chrome shown (Tab)"
				                                                  : "chrome hidden (Tab)");
			}
			containerWindow->hotkeysHandler.key_callback(window, key, scancode, action, mods);
		}
	}
};
