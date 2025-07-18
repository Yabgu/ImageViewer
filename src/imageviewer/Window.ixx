module;

#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cassert>
#include <format>
#include <thread>
#include <filesystem>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

import TexturePool;
import Image;
import HotkeysHandler;
export module UserInterface;

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

	void LoadTextures(
		const Image& image,
		const int segmentWidth,
		const int segmentHeight,
		const int redundantBorderSize)
	{
		this->textureCollection.reset(new TextureCollection(image, segmentWidth, segmentHeight, redundantBorderSize));
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

		if (textureCollection != nullptr)
		{
			double zoom = getZoom();
			float imgW = textureCollection->width * zoom;
			float imgH = textureCollection->height * zoom;
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
			textureCollection->Draw(modelLoc);
		}
		glfwSwapBuffers(glfwWindow);
	}

	void Main()
	{
		glfwMakeContextCurrent(this->glfwWindow);
		while (!glfwWindowShouldClose(this->glfwWindow))
		{
			[[likely]]
			Draw();
			glfwWaitEventsTimeout(1.0 / 60);
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
