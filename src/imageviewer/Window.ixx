module;

#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <string>

#include "RendererPluginDef.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

import Image;
import HotkeysHandler;
export module UserInterface;

// ---------------------------------------------------------------------------
// RendererPlugin — loads the renderer shared library and exposes its interface
// ---------------------------------------------------------------------------

struct RendererPlugin {
#ifdef _WIN32
	using PlatformHandle = HMODULE;
#else
	using PlatformHandle = void*;
#endif

	PlatformHandle handle{};
	Renderer_SetWindowHintsFunc setWindowHintsFunc{};
	Renderer_CreateFunc         createFunc{};
	Renderer_DestroyFunc        destroyFunc{};
	Renderer_ResizeFunc         resizeFunc{};
	Renderer_LoadTexturesFunc   loadTexturesFunc{};
	Renderer_FreeTexturesFunc   freeTexturesFunc{};
	Renderer_DrawFunc           drawFunc{};

	RendererPlugin()
	{
#ifdef _WIN32
		handle = LoadLibraryW(L"RendererOpenGL.dll");
		if (!handle)
			throw std::runtime_error("Failed to load RendererOpenGL.dll");
#else
		handle = dlopen("libRendererOpenGL.so", RTLD_LAZY);
		if (!handle) {
			// Try the directory containing the current executable
			std::filesystem::path exeDir;
#if defined(__linux__)
			exeDir = std::filesystem::canonical("/proc/self/exe").parent_path();
#elif defined(__APPLE__)
			{
				char buf[4096];
				uint32_t size = sizeof(buf);
				if (_NSGetExecutablePath(buf, &size) == 0)
					exeDir = std::filesystem::canonical(buf).parent_path();
			}
#endif
			auto altPath = exeDir / "libRendererOpenGL.so";
			handle = dlopen(altPath.c_str(), RTLD_LAZY);
		}
		if (!handle)
			throw std::runtime_error(std::string("Failed to load libRendererOpenGL.so: ") + dlerror());
#endif
		loadProc(setWindowHintsFunc, "Renderer_SetWindowHints");
		loadProc(createFunc,         "Renderer_Create");
		loadProc(destroyFunc,        "Renderer_Destroy");
		loadProc(resizeFunc,         "Renderer_Resize");
		loadProc(loadTexturesFunc,   "Renderer_LoadTextures");
		loadProc(freeTexturesFunc,   "Renderer_FreeTextures");
		loadProc(drawFunc,           "Renderer_Draw");
	}

	~RendererPlugin()
	{
		if (handle) {
#ifdef _WIN32
			FreeLibrary(handle);
#else
			dlclose(handle);
#endif
		}
	}

	RendererPlugin(const RendererPlugin&)            = delete;
	RendererPlugin& operator=(const RendererPlugin&) = delete;

	void setWindowHints() const { setWindowHintsFunc(); }

	RendererHandle create(void* glfwWindow) const { return createFunc(glfwWindow); }

	void destroy(RendererHandle r) const { destroyFunc(r); }

	void resize(RendererHandle r, int w, int h) const { resizeFunc(r, w, h); }

	TextureCollectionHandle loadTextures(
		RendererHandle r,
		const uint8_t* data, int w, int h, int cpp,
		int segW, int segH, int border) const
	{
		return loadTexturesFunc(r, data, w, h, cpp, segW, segH, border);
	}

	void freeTextures(RendererHandle r, TextureCollectionHandle tc) const
	{
		freeTexturesFunc(r, tc);
	}

	void draw(RendererHandle r, TextureCollectionHandle tc,
		int winW, int winH, double zoom, double panX, double panY) const
	{
		drawFunc(r, tc, winW, winH, zoom, panX, panY);
	}

private:
	template<typename FuncPtr>
	void loadProc(FuncPtr& out, const char* name)
	{
#ifdef _WIN32
		out = reinterpret_cast<FuncPtr>(GetProcAddress(handle, name));
#else
		out = reinterpret_cast<FuncPtr>(dlsym(handle, name));
#endif
		if (!out)
			throw std::runtime_error(std::string("Failed to resolve renderer function: ") + name);
	}
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

export class Window
{
protected:
	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (win != nullptr) {
			win->width  = width;
			win->height = height;
			if (win->rendererHandle)
				win->rendererPlugin.resize(win->rendererHandle, width, height);
		}
	}

	static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
	{
		window_size_callback(window, width, height);
	}

	static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
	{
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win != nullptr)
			win->scroll_callback(xoffset, yoffset);
	}

	void scroll_callback(double xoffset, double yoffset)
	{
		constexpr int MIN_ZOOM_PERCENT = -90;
		constexpr int MAX_ZOOM_PERCENT = 400;
		constexpr double SCROLL_SENSITIVITY = 10;

		double mouseX, mouseY;
		glfwGetCursorPos(glfwWindow, &mouseX, &mouseY);

		double newZoomLevel = (scroll + yoffset * SCROLL_SENSITIVITY + 100.0) / 100.0;
		newZoomLevel = std::clamp(newZoomLevel,
			(MIN_ZOOM_PERCENT + 100.0) / 100.0,
			(MAX_ZOOM_PERCENT + 100.0) / 100.0);

		setZoom(newZoomLevel, mouseX, mouseY);
	}

private:
	// Declared in construction order (member initialiser list depends on this order)
	RendererPlugin          rendererPlugin;
	GLFWwindow* const       glfwWindow;
	int                     width;
	int                     height;
	int                     scroll;
	RendererHandle          rendererHandle;
	TextureCollectionHandle textureHandle;
	int                     imageWidth;
	int                     imageHeight;
	double panX      = 0.0;
	double panY      = 0.0;
	bool   isPanning = false;
	double lastMouseX = 0.0, lastMouseY = 0.0;

	double getZoom() const { return (scroll + 100.0) / 100.0; }

	void setPanning(double x, double y)
	{
		if (textureHandle) {
			double zoom = getZoom();
			double imgW = imageWidth  * zoom;
			double imgH = imageHeight * zoom;
			double minPanX = -(imgW - width)  * 0.5;
			double maxPanX =  (imgW - width)  * 0.5;
			double minPanY = -(imgH - height) * 0.5;
			double maxPanY =  (imgH - height) * 0.5;

			panX = (imgW < width)  ? 0.0 : std::max(minPanX, std::min(x, maxPanX));
			panY = (imgH < height) ? 0.0 : std::max(minPanY, std::min(y, maxPanY));
		} else {
			panX = x;
			panY = y;
		}
	}

	static void mouse_button_callback(GLFWwindow* window, int button, int action, int /*mods*/)
	{
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (button == GLFW_MOUSE_BUTTON_LEFT) {
			if (action == GLFW_PRESS) {
				double x, y;
				glfwGetCursorPos(window, &x, &y);
				win->isPanning  = true;
				win->lastMouseX = x;
				win->lastMouseY = y;
			} else if (action == GLFW_RELEASE) {
				win->isPanning = false;
			}
		}
	}

	static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos)
	{
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (win->isPanning) {
			double dx = xpos - win->lastMouseX;
			double dy = ypos - win->lastMouseY;
			win->setPanning(win->panX + dx, win->panY + dy);
			win->lastMouseX = xpos;
			win->lastMouseY = ypos;
		}
	}

	// Creates the GLFW window and makes its context current.
	// glad initialisation and shader setup are delegated to Renderer_Create.
	static GLFWwindow* InitializeWindow(int width, int height, Window* containerWindow,
	                                    const RendererPlugin& renderer)
	{
		renderer.setWindowHints();

		if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND))
			glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

		GLFWwindow* const win = glfwCreateWindow(width, height, "ImageViewer", NULL, NULL);
		if (win == nullptr)
			throw std::runtime_error("Failed to create window");

		glfwSetWindowUserPointer(win, (void*)containerWindow);
		glfwMakeContextCurrent(win);

		glfwSetWindowSizeCallback(win,      window_size_callback);
		glfwSetFramebufferSizeCallback(win, framebuffer_size_callback);
		glfwSetScrollCallback(win,          scroll_callback);
		glfwSetMouseButtonCallback(win,     mouse_button_callback);
		glfwSetCursorPosCallback(win,       cursor_position_callback);
		glfwSetKeyCallback(win,             key_callback);

		return win;
	}

public:
	inline static void Initialize()
	{
		if (!glfwInit())
			[[unlikely]] throw std::runtime_error("glfwInit failed");
	}

	static void Deinitialize()
	{
		glfwTerminate();
	}

private:
	HotkeysHandler hotkeysHandler;

public:
	Window(int width, int height)
		: rendererPlugin(),
		  glfwWindow(InitializeWindow(width, height, this, rendererPlugin)),
		  width(width),
		  height(height),
		  scroll(0),
		  rendererHandle(rendererPlugin.create(glfwWindow)),
		  textureHandle(nullptr),
		  imageWidth(0),
		  imageHeight(0),
		  hotkeysHandler()
	{
		// Set the initial viewport now that the renderer is ready
		rendererPlugin.resize(rendererHandle, width, height);

		hotkeysHandler.setZoomCallback([this](int zoomDelta) { adjustZoom(zoomDelta); });
		hotkeysHandler.setPanCallback([this](int dx, int dy) { adjustPan(dx, dy); });
		hotkeysHandler.setSetZoomCallback([this](double zoomLevel) { setZoom(zoomLevel); });
		hotkeysHandler.setFitToWindowCallback([this]() { fitToWindow(); });
	}

	~Window()
	{
		if (textureHandle)
			rendererPlugin.freeTextures(rendererHandle, textureHandle);
		rendererPlugin.destroy(rendererHandle);
	}

	void CenterImage()
	{
		setPanning(0.0, 0.0);
		if (textureHandle) {
			double minRatio = std::min(
				(double)width  / imageWidth,
				(double)height / imageHeight);
			scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
	}

	void LoadTextures(
		const Image& image,
		const int segmentWidth,
		const int segmentHeight,
		const int redundantBorderSize)
	{
		if (textureHandle) {
			rendererPlugin.freeTextures(rendererHandle, textureHandle);
			textureHandle = nullptr;
		}
		imageWidth  = image.width;
		imageHeight = image.height;
		textureHandle = rendererPlugin.loadTextures(
			rendererHandle,
			image.data, image.width, image.height, image.componentsPerPixel,
			segmentWidth, segmentHeight, redundantBorderSize);
		CenterImage();
	}

	void Draw()
	{
		rendererPlugin.draw(rendererHandle, textureHandle,
			width, height, getZoom(), panX, panY);
	}

	void Main()
	{
		while (!glfwWindowShouldClose(glfwWindow)) {
			[[likely]]
			Draw();
			glfwWaitEventsTimeout(1.0 / 60);
		}
	}

public:
	void setZoom(double zoomLevel)
	{
		setZoom(zoomLevel, width * 0.5, height * 0.5);
	}

	void setZoom(double zoomLevel, double cursorX, double cursorY)
	{
		const double currentZoom = getZoom();
		scroll = std::clamp(static_cast<int>(zoomLevel * 100.0 - 100.0), -90, 400);

		if (textureHandle) {
			const double imgW = imageWidth  * currentZoom;
			const double imgH = imageHeight * currentZoom;
			const double imgX = (width  - imgW) * 0.5 + panX;
			const double imgY = (height - imgH) * 0.5 + panY;

			const double mouseImgXBefore = (cursorX - imgX) / currentZoom;
			const double mouseImgYBefore = (cursorY - imgY) / currentZoom;

			const double newZoom = getZoom();
			const double newImgW = imageWidth  * newZoom;
			const double newImgH = imageHeight * newZoom;
			const double newImgX = (width  - newImgW) * 0.5 + panX;
			const double newImgY = (height - newImgH) * 0.5 + panY;
			const double mouseScreenXAfter = newImgX + mouseImgXBefore * newZoom;
			const double mouseScreenYAfter = newImgY + mouseImgYBefore * newZoom;

			setPanning(panX + cursorX - mouseScreenXAfter,
			           panY + cursorY - mouseScreenYAfter);
		}
	}

	void adjustZoom(int zoomDelta)
	{
		const double currentZoom = getZoom();
		scroll = std::clamp(scroll + zoomDelta, -90, 400);

		if (textureHandle) {
			const double imgW = imageWidth  * currentZoom;
			const double imgH = imageHeight * currentZoom;
			const double imgX = (width  - imgW) * 0.5 + panX;
			const double imgY = (height - imgH) * 0.5 + panY;

			const double centerX = width  * 0.5;
			const double centerY = height * 0.5;
			const double mouseImgXBefore = (centerX - imgX) / currentZoom;
			const double mouseImgYBefore = (centerY - imgY) / currentZoom;

			const double newZoom = getZoom();
			const double newImgW = imageWidth  * newZoom;
			const double newImgH = imageHeight * newZoom;
			const double newImgX = (width  - newImgW) * 0.5 + panX;
			const double newImgY = (height - newImgH) * 0.5 + panY;
			const double mouseScreenXAfter = newImgX + mouseImgXBefore * newZoom;
			const double mouseScreenYAfter = newImgY + mouseImgYBefore * newZoom;

			setPanning(panX + centerX - mouseScreenXAfter,
			           panY + centerY - mouseScreenYAfter);
		}
	}

	void adjustPan(int dx, int dy)
	{
		setPanning(panX + dx, panY + dy);
	}

	void fitToWindow()
	{
		setPanning(0.0, 0.0);
		if (textureHandle) {
			const double minRatio = std::min(
				(double)width  / imageWidth,
				(double)height / imageHeight);
			scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
	}

	static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
	{
		Window* containerWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (containerWindow)
			containerWindow->hotkeysHandler.key_callback(window, key, scancode, action, mods);
	}
};
