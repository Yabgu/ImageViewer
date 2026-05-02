module;

#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <format>
#include <thread>
#include <chrono>
#include <filesystem>
#include <algorithm>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "FilterPluginDef.h"

export module UserInterface;

import TexturePool;
import Image;
import HotkeysHandler;
import FilterPlugin;

export class Window
{
protected:
	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (win != nullptr)
		{
			win->width = width;
			win->height = height;
		}

		glViewport(0, 0, width, height);
	}

	static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
	{
		window_size_callback(window, width, height);
	}

	static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
	{
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win != nullptr)
		{
			win->scroll_callback(xoffset, yoffset);
		}
	}

	void scroll_callback(double xoffset, double yoffset)
	{
		constexpr int MIN_ZOOM_PERCENT = -90;
		constexpr int MAX_ZOOM_PERCENT = 400;
		constexpr double SCROLL_SENSITIVITY = 10;

		// Get mouse position relative to window
		double mouseX, mouseY;
		glfwGetCursorPos(glfwWindow, &mouseX, &mouseY);

		// Update scroll value based on scroll input
		double newZoomLevel = (scroll + yoffset * SCROLL_SENSITIVITY + 100.0) / 100.0;
		newZoomLevel = std::clamp(newZoomLevel, (MIN_ZOOM_PERCENT + 100.0) / 100.0, (MAX_ZOOM_PERCENT + 100.0) / 100.0);

		// Use setZoom to handle zooming with cursor position
		setZoom(newZoomLevel, mouseX, mouseY);
	}

private:
	GLFWwindow* const glfwWindow;
	int width;
	int height;
	int scroll;
	std::shared_ptr<TextureCollection> textureCollection;
	/* PWM frame set: non-empty when filter plugin produced multiple frames.
	 * textureCollection always points to pwmFrames_[0] when the vector is
	 * non-empty, so the rest of the code (panning, zooming, …) keeps working
	 * without change.  Draw() cycles through pwmFrames_ at 60 fps. */
	std::vector<std::shared_ptr<TextureCollection>> pwmFrames_;
	mutable uint64_t frameCounter_ = 0;
	FilterPlugin filterPlugin_;
	GLuint shaderProgram;
	double panX = 0.0;
	double panY = 0.0;
	bool isPanning = false;
	double lastMouseX = 0.0, lastMouseY = 0.0;

	double getZoom() const {
		return (scroll + 100.0) / 100.0;
	}

	void setPanning(double x, double y) {
		if (textureCollection) {
			double zoom = getZoom();
			double imgW = textureCollection->width * zoom;
			double imgH = textureCollection->height * zoom;
			double minPanX = -(imgW - width) * 0.5;
			double maxPanX = (imgW - width) * 0.5;
			double minPanY = -(imgH - height) * 0.5;
			double maxPanY = (imgH - height) * 0.5;

			// Center the image if it is smaller than the window and centering is requested
			if (imgW < width) {
				panX = 0.0;
			} else {
				panX = std::max(minPanX, std::min(x, maxPanX));
			}

			if (imgH < height) {
				panY = 0.0;
			} else {
				panY = std::max(minPanY, std::min(y, maxPanY));
			}
		} else {
			panX = x;
			panY = y;
		}
	}

	static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (button == GLFW_MOUSE_BUTTON_LEFT) {
			if (action == GLFW_PRESS) {
				double x, y;
				glfwGetCursorPos(window, &x, &y);
				win->isPanning = true;
				win->lastMouseX = x;
				win->lastMouseY = y;
			} else if (action == GLFW_RELEASE) {
				win->isPanning = false;
			}
		}
	}
	static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
		Window* win = (Window*)glfwGetWindowUserPointer(window);
		if (win->isPanning) {
			double dx = xpos - win->lastMouseX;
			double dy = ypos - win->lastMouseY;
			win->setPanning(win->panX + dx, win->panY + dy);
			win->lastMouseX = xpos;
			win->lastMouseY = ypos;
		}
	}

	static GLFWwindow* InitializeWindow(int width, int height, Window* containerWindow)
	{
		// Check that the window was successfully created
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);

		
		if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND))
		{
			glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
		}

		GLFWwindow* const glfwWindow = glfwCreateWindow(width, height, "ImageViewer", NULL, NULL);
		if (glfwWindow == nullptr)
		{
			throw std::runtime_error("Failed to create window");
		}

		glfwSetWindowUserPointer(glfwWindow, (void*)containerWindow);

		glfwMakeContextCurrent(glfwWindow);

		glfwSetWindowSizeCallback(glfwWindow, window_size_callback);
		glfwSetFramebufferSizeCallback(glfwWindow, framebuffer_size_callback);
		glfwSetScrollCallback(glfwWindow, scroll_callback);
		glfwSetMouseButtonCallback(glfwWindow, mouse_button_callback);
		glfwSetCursorPosCallback(glfwWindow, cursor_position_callback);
		glfwSetKeyCallback(glfwWindow, key_callback);

		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			[[unlikely]] throw std::runtime_error("gladLoadGLLoader failed");
		}

		glEnable(GL_TEXTURE_2D);

		// Resize the window
		window_size_callback(glfwWindow, width, height);
		return glfwWindow;
	}

	static GLuint initializeDefaultShaderProgram() {
		// --- Shader and VAO/VBO setup (new for OpenGL 4.x) ---
		// Basic shaders for rendering a textured quad.
		// In a real application, these would be loaded from files.
		const char* vertexShaderSource = R"(
			#version 400 core
			layout (location = 0) in vec2 aPos;
			layout (location = 1) in vec2 aTexCoord;

			out vec2 TexCoord;

			uniform mat4 projection; // Orthographic projection matrix
			uniform mat4 model;	  // Model matrix for position

			void main()
			{
				gl_Position = projection * model * vec4(aPos.x, aPos.y, 0.0, 1.0);
				TexCoord = aTexCoord;
			}
		)";

		const char* fragmentShaderSource = R"(
			#version 400 core
			out vec4 FragColor;

			in vec2 TexCoord;

			uniform sampler2D ourTexture;

			void main()
			{
				FragColor = texture(ourTexture, TexCoord);
			}
		)";

		GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
		GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
		return LinkProgram(vertexShader, fragmentShader);
	}

	// Helper function to create and compile a shader
	static GLuint CompileShader(GLenum type, const char* source)
	{
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, NULL);
		glCompileShader(shader);

		int success;
		char infoLog[512];
		glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
		if (!success)
		{
			glGetShaderInfoLog(shader, 512, NULL, infoLog);
			throw std::runtime_error(std::string("Shader compilation failed: ") + infoLog);
		}
		return shader;
	}

	// Helper function to link a shader program
	static GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader)
	{
		GLuint program = glCreateProgram();
		glAttachShader(program, vertexShader);
		glAttachShader(program, fragmentShader);
		glLinkProgram(program);

		int success;
		char infoLog[512];
		glGetProgramiv(program, GL_LINK_STATUS, &success);
		if (!success)
		{
			glGetProgramInfoLog(program, 512, NULL, infoLog);
			throw std::runtime_error(std::string("Shader linking failed: ") + infoLog);
		}
		glDeleteShader(vertexShader);
		glDeleteShader(fragmentShader);
		return program;
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
			shaderProgram(initializeDefaultShaderProgram()),
			hotkeysHandler() {
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
		if (shaderProgram) glDeleteProgram(shaderProgram);
	}

	void CenterImage()
	{
		setPanning(0.0, 0.0);
		if (textureCollection) {
			double minRatio = std::min<double>((double)width / textureCollection->width, (double)height / textureCollection->height);
			this->scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
	}

	/* ── Filter-plugin helpers ──────────────────────────────────────────── */

	/* Query how many bits per colour channel the current GL framebuffer has. */
	static IWScreenInfo QueryScreenInfo() noexcept
	{
		GLint redBits = 8;
		glGetFramebufferAttachmentParameteriv(
			GL_FRAMEBUFFER, GL_BACK_LEFT,
			GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redBits);
		if (redBits <= 0) redBits = 8;
		return { static_cast<uint8_t>(redBits), 4u };
	}

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

	/* Create a TextureCollection from an ImagePluginData frame.
	 * Makes a full copy of the pixel data so the filter plugin's malloc
	 * allocation is not touched by Image's delete[]-based destructor. */
	static std::shared_ptr<TextureCollection> MakeCollection(
		const ImagePluginData& pd, int segW, int segH, int border)
	{
		Image tmp(pd.width, pd.height, pd.format, pd.colorSpace);
		if (pd.stride == tmp.stride) {
			std::memcpy(tmp.data, pd.data, tmp.size);
		} else {
			const int copyBytes = std::min(pd.stride, tmp.stride);
			for (int y = 0; y < pd.height; ++y) {
				std::memcpy(tmp.data + static_cast<ptrdiff_t>(y) * tmp.stride,
				            pd.data  + static_cast<ptrdiff_t>(y) * pd.stride,
				            copyBytes);
			}
		}
		return std::make_shared<TextureCollection>(tmp, segW, segH, border);
	}

public:
	void LoadTextures(
		const Image& image,
		const int segmentWidth,
		const int segmentHeight,
		const int redundantBorderSize,
		const IWFilterOptions& opts = IWFilterOptions{
			IW_FILTER_OPTIONS_VERSION, IW_DITHER_NONE, 0u })
	{
		pwmFrames_.clear();
		frameCounter_ = 0;

		if (filterPlugin_.IsLoaded()) {
			const ImagePluginData pd   = MakePluginDataView(image);
			const IWScreenInfo  screen = QueryScreenInfo();

			IWFilterImageSet* set = nullptr;
			try {
				set = filterPlugin_.Filter(&pd, &screen, &opts);
			} catch (...) {
				set = nullptr;
			}

			if (set && set->frameCount > 0 && set->frames) {
				for (uint32_t i = 0; i < set->frameCount; ++i) {
					if (!set->frames[i]) continue;
					try {
						pwmFrames_.push_back(
							MakeCollection(*set->frames[i],
							               segmentWidth,
							               segmentHeight,
							               redundantBorderSize));
					} catch (...) {
						/* If one frame fails, skip it. */
					}
				}
				filterPlugin_.Free(set);
			} else {
				if (set) filterPlugin_.Free(set);
			}
		}

		if (pwmFrames_.empty()) {
			/* Fallback: no filter or filter returned nothing — use image directly. */
			pwmFrames_.push_back(std::make_shared<TextureCollection>(
				image, segmentWidth, segmentHeight, redundantBorderSize));
		}

		textureCollection = pwmFrames_[0];
		CenterImage();
	}

	void Draw() const
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glLoadIdentity();
		glUseProgram(shaderProgram);

		// Get uniform locations
		GLint projectionLoc = glGetUniformLocation(shaderProgram, "projection");
		GLint modelLoc = glGetUniformLocation(shaderProgram, "model");
		GLint textureSamplerLoc = glGetUniformLocation(shaderProgram, "ourTexture");

		// Set the texture unit for the sampler
		glUniform1i(textureSamplerLoc, 0); // Corresponds to GL_TEXTURE0

		/* Select the active frame (PWM cycles through all frames). */
		TextureCollection* activeTc = nullptr;
		if (!pwmFrames_.empty()) {
			const size_t idx = (pwmFrames_.size() > 1u)
			                 ? (frameCounter_ % pwmFrames_.size())
			                 : 0u;
			++frameCounter_;
			activeTc = pwmFrames_[idx].get();
		}

		if (activeTc != nullptr)
		{
			double zoom = getZoom();
			float imgW = activeTc->width * zoom;
			float imgH = activeTc->height * zoom;
			float cx = (width - imgW) * 0.5f;
			float cy = (height - imgH) * 0.5f;
			float left = (-cx - panX) / zoom;
			float right = (width - cx - panX) / zoom;
			float top = (-cy - panY) / zoom;
			float bottom = (height - cy - panY) / zoom;
			float nearVal = -1.0f;
			float farVal = 1.0f;
			float ortho[16] = {
				2.0f / (right - left), 0.0f, 0.0f, 0.0f,
				0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
				0.0f, 0.0f, -2.0f / (farVal - nearVal), 0.0f,
				-(right + left) / (right - left), -(top + bottom) / (top - bottom), -(farVal + nearVal) / (farVal - nearVal), 1.0f
			};
			glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, ortho);
			activeTc->Draw(modelLoc);
		}
		glfwSwapBuffers(glfwWindow);
	}

	void Main()
	{
		glfwMakeContextCurrent(this->glfwWindow);

		using Clock    = std::chrono::steady_clock;
		using Duration = std::chrono::duration<double>;
		constexpr double kFrameSeconds = 1.0 / 60.0;

		auto lastFrame = Clock::now();

		while (!glfwWindowShouldClose(this->glfwWindow))
		{
			[[likely]]
			Draw();

			if (pwmFrames_.size() > 1u) {
				/* PWM active: poll events and sleep until the next 60 fps deadline
				 * so each frame gets an equal display duration. */
				glfwPollEvents();
				std::this_thread::sleep_until(lastFrame + Duration(kFrameSeconds));
				lastFrame = Clock::now();
			} else {
				/* Single frame: wait for events (power-efficient). */
				glfwWaitEventsTimeout(kFrameSeconds);
				lastFrame = Clock::now();
			}
		}
	}

public:
	void setZoom(double zoomLevel) {
		// Use the center of the window as the default cursor position
		setZoom(zoomLevel, width * 0.5, height * 0.5);
	}

	void setZoom(double zoomLevel, double cursorX, double cursorY) {
        const double currentZoom = getZoom();
        const double newScroll = (zoomLevel * 100.0) - 100.0;
        scroll = std::clamp(static_cast<int>(newScroll), -90, 400);

        if (textureCollection) {
            const double imgW = textureCollection->width * currentZoom;
            const double imgH = textureCollection->height * currentZoom;
            const double imgX = (width - imgW) * 0.5 + panX;
            const double imgY = (height - imgH) * 0.5 + panY;

            // Calculate the image position relative to the cursor
            const double mouseImgXBefore = (cursorX - imgX) / currentZoom;
            const double mouseImgYBefore = (cursorY - imgY) / currentZoom;

            const double newZoom = getZoom();
            const double newImgW = textureCollection->width * newZoom;
            const double newImgH = textureCollection->height * newZoom;
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

        if (textureCollection) {
            const double imgW = textureCollection->width * currentZoom;
            const double imgH = textureCollection->height * currentZoom;
            const double imgX = (width - imgW) * 0.5 + panX;
            const double imgY = (height - imgH) * 0.5 + panY;

            // Calculate the center of the current panning
            const double centerX = width * 0.5;
            const double centerY = height * 0.5;
            const double mouseImgXBefore = (centerX - imgX) / currentZoom;
            const double mouseImgYBefore = (centerY - imgY) / currentZoom;

            const double newZoom = getZoom();
            const double newImgW = textureCollection->width * newZoom;
            const double newImgH = textureCollection->height * newZoom;
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
		if (textureCollection) {
			const double minRatio = std::min(static_cast<double>(width) / textureCollection->width,
											 static_cast<double>(height) / textureCollection->height);
			scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
	}

	static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
		Window* containerWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (containerWindow) {
			containerWindow->hotkeysHandler.key_callback(window, key, scancode, action, mods);
		}
	}
};
