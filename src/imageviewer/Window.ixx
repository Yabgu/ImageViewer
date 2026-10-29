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
		/* Leave the wheel to the ImGui chrome when the cursor is over it. */
		if (win != nullptr && !ImguiWantsMouse())
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
	Window(int width, int height)
		: glfwWindow(InitializeWindow(width, height, this)),
		  width(width),
		  height(height),
		  scroll(0),
		  hotkeysHandler() {
		/* Diagnostics must interleave truthfully with the driver's own stderr. */
		std::setvbuf(stderr, nullptr, _IONBF, 0);
		IviewTrace("window created");

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

public:
	/* Kept under its OpenGL name.  Runs the filter plugin (which does the actual
	 * temporal quantisation) and uploads at most two phase frames; returns false
	 * when the plugin produced more than two frames — N-phase is out of scope. */
	bool LoadTextures(
		const Image& image,
		const int /*segmentWidth*/,
		const int /*segmentHeight*/,
		const int /*redundantBorderSize*/,
		const IWFilterOptions& opts = IWFilterOptions{
			IW_FILTER_OPTIONS_VERSION, 0u, nullptr })
	{
		if (image.width <= 0 || image.height <= 0)
			return false;

		std::vector<uint8_t> phaseA;
		std::vector<uint8_t> phaseB;

		if (filterPlugin_.IsLoaded()) {
			const ImagePluginData pd = MakePluginDataView(image);
			/* 8 bits per channel, 4 channels: what the default framebuffer offers. */
			const IWScreenInfo screen{ 8u, 4u };

			IWFilterImageSet* set = nullptr;
			try { set = filterPlugin_.Filter(&pd, &screen, &opts); }
			catch (...) { set = nullptr; }

			if (set && set->frameCount > 0 && set->frames) {
				if (set->frameCount > 2u) {
					std::fprintf(stderr,
						"iview: pwm_bits > 1 (%u phase frames) is not implemented in this build\n",
						set->frameCount);
					filterPlugin_.Free(set);
					return false;
				}
				for (uint32_t i = 0; i < set->frameCount; ++i) {
					if (!set->frames[i]) continue;
					try {
						if (i == 0) ToRgba8(*set->frames[i], phaseA);
						else        ToRgba8(*set->frames[i], phaseB);
					} catch (...) { /* skip a frame that fails to convert */ }
				}
				filterPlugin_.Free(set);
			} else if (set) {
				filterPlugin_.Free(set);
			}
		}

		if (phaseA.empty()) {
			/* No filter (or it returned nothing): use the image directly. */
			ToRgba8(MakePluginDataView(image), phaseA);
		}

		UploadTextures(phaseA, phaseB, image.width, image.height);

		imageWidth_  = image.width;
		imageHeight_ = image.height;
		CenterImage();
		return true;
	}

	/* Filter options from the command line.  The argument array is copied into
	 * the Window, so the caller's storage does not have to outlive Main(). */
	void SetFilterOptions(const IWFilterOptions& opts)
	{
		const uint32_t count = std::min<uint32_t>(opts.argc, 5u);
		for (uint32_t i = 0; i < count; ++i) filterArgStorage_[i] = opts.argv[i];
		filterOptions_.version = opts.version;
		filterOptions_.argc    = count;
		filterOptions_.argv    = (count != 0u) ? filterArgStorage_ : nullptr;
	}

	void CenterImage()
	{
		setPanning(0.0, 0.0);
		if (HasImage()) {
			const double minRatio = std::min<double>((double)width / imageWidth_,
			                                         (double)height / imageHeight_);
			this->scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
	}

	/* ── Main loop ─────────────────────────────────────────────────────────── */

	/* Draws the image quad for one phase.  The transform is built in NDC on the
	 * CPU, so the shader keeps the same projection*model pipeline as the OpenGL
	 * branch while needing no per-frame buffer updates. */
	void DrawImage(uint32_t phase) const
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, width, height);
		glClearColor(kCanvasBg[0], kCanvasBg[1], kCanvasBg[2], 1.0f);
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
					const Image image = imageFuture.get();
					if (!LoadTextures(image, 0, 0, 0, filterOptions_))
						throw std::runtime_error("the filter produced an unsupported frame count");
					SetWindowTitle(imagePath);
					ComputeReadout(image, imagePath);
					uploaded = true;
					IviewTrace("image uploaded to the GPU");
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
				glClearColor(kCanvasBg[0], kCanvasBg[1], kCanvasBg[2], 1.0f);
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

		ShutdownImGui();
	}

private:
	/* True while the chrome alpha still has to move towards its target. */
	bool ChromeNeedsFrame() const
	{
		if (!imgui_) return false;
		using Clock = std::chrono::steady_clock;
		const auto now = Clock::now();
		const bool idle = (now - imgui_->lastMouseActivity) > std::chrono::milliseconds(1500);
		const float target = !imgui_->chromeEnabled ? 0.0f : (idle ? 0.15f : 1.0f);
		return std::fabs(imgui_->alpha - target) > 0.0001f;
	}

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
			imgui_->readout = std::format("{}\xC3\x97{}", image.width, image.height);
		}
		else
		{
			const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
			imgui_->readout = std::format("{}\xC3\x97{} \xC2\xB7 {:.1f} MB",
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

		if (!imgui_->frameClockStarted)
		{
			imgui_->lastMouseActivity = now;
			imgui_->frameClockStarted = true;
		}

		const ImGuiIO& io = ImGui::GetIO();
		if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
			imgui_->lastMouseActivity = now;

		const bool idle = (now - imgui_->lastMouseActivity) > std::chrono::milliseconds(1500);
		float target = 0.15f;
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
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + 10.0f),
		                        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
		if (ImGui::Begin("##readout", nullptr, kChromeFlags | ImGuiWindowFlags_NoInputs))
		{
			ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
			ImGui::TextUnformatted(imgui_->readout.c_str());
			ImGui::PopFont();
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		/* Bottom-centre control strip. */
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
		                               vp->WorkPos.y + vp->WorkSize.y - 12.0f),
		                        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
		if (ImGui::Begin("##controls", nullptr, kChromeFlags))
		{
			if (ImGui::Button("Fit")) fitToWindow();
			ImGui::SameLine();
			if (ImGui::Button("1:1")) setZoom(1.0);

			ImGui::SameLine(0.0f, 14.0f);
			if (ImGui::Button("-")) adjustZoom(-10);
			ImGui::SameLine();
			char zoomLabel[16];
			std::snprintf(zoomLabel, sizeof(zoomLabel), "%3d%%", getZoomPercent());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(zoomLabel);
			ImGui::SameLine();
			if (ImGui::Button("+")) adjustZoom(10);

			ImGui::SameLine(0.0f, 14.0f);
			if (ImGui::Button("Filter")) ImGui::OpenPopup("##filters");
			if (ImGui::BeginPopup("##filters"))
			{
				ImGui::TextDisabled("No filters yet");
				ImGui::EndPopup();
			}
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}

	/* File drop: phase 1 reuses the same decode + upload path on the main
	 * thread (this runs inside glfwPollEvents, between two frames). */
	void HandleDrop(int count, const char** paths)
	{
		if (count <= 0 || paths == nullptr || !imgui_) return;

		for (int i = 0; i < count; ++i)
		{
			if (paths[i] == nullptr) continue;
			try
			{
				const std::filesystem::path path(reinterpret_cast<const char8_t*>(paths[i]));
				const Image image = Image::FromFile(path);
				if (!LoadTextures(image, 0, 0, 0, filterOptions_))
					continue;
				SetWindowTitle(path);
				ComputeReadout(image, path);
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
