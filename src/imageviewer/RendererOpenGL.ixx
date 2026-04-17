module;

#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "RendererPluginDef.h"

export module RendererOpenGL;

namespace {

// ---------------------------------------------------------------------------
// Internal pixel-subsection helper (replaces Image::Subsection)
// ---------------------------------------------------------------------------

struct SubImage {
	int width;
	int height;
	int componentsPerPixel;
	int stride;
	uint8_t* data;

	SubImage(int w, int h, int cpp)
		: width(w), height(h), componentsPerPixel(cpp),
		  stride(w * cpp), data(new uint8_t[(size_t)w * h * cpp]) {}

	~SubImage() { delete[] data; }
	SubImage(const SubImage&) = delete;
	SubImage& operator=(const SubImage&) = delete;
};

static SubImage* MakeSubsection(
	const uint8_t* srcData, int srcWidth, int srcHeight, int srcCpp,
	int x, int y, int w, int h)
{
	SubImage* res = new SubImage(w, h, srcCpp);
	std::fill_n(res->data, (size_t)w * h * srcCpp, uint8_t{0});

	if (x >= srcWidth || y >= srcHeight || -x >= w || -y >= h)
		return res;

	const int copyW = std::min(w, srcWidth - x);
	const int copyH = std::min(h, srcHeight - y);
	const int srcStride = srcWidth * srcCpp;
	const int copyStride = copyW * srcCpp;

	for (int py = 0; py < copyH; ++py) {
		std::memcpy(
			res->data + py * res->stride,
			srcData + x * srcCpp + (y + py) * srcStride,
			copyStride);
	}
	return res;
}

// ---------------------------------------------------------------------------
// Shader helpers
// ---------------------------------------------------------------------------

static GLuint CompileShader(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	int success;
	char infoLog[512];
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		glGetShaderInfoLog(shader, 512, NULL, infoLog);
		throw std::runtime_error(std::string("Shader compilation failed: ") + infoLog);
	}
	return shader;
}

static GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader)
{
	GLuint program = glCreateProgram();
	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);
	glLinkProgram(program);

	int success;
	char infoLog[512];
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		glGetProgramInfoLog(program, 512, NULL, infoLog);
		throw std::runtime_error(std::string("Shader linking failed: ") + infoLog);
	}
	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);
	return program;
}

static GLuint BuildDefaultShaderProgram()
{
	const char* vertexShaderSource = R"(
		#version 400 core
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
		#version 400 core
		out vec4 FragColor;

		in vec2 TexCoord;

		uniform sampler2D ourTexture;

		void main()
		{
			FragColor = texture(ourTexture, TexCoord);
		}
	)";

	GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
	return LinkProgram(vs, fs);
}

// ---------------------------------------------------------------------------
// TextureCollection — tiled GPU texture store
// ---------------------------------------------------------------------------

struct TextureCollection
{
public:
	const int width;
	const int height;

private:
	const int segmentWidth;
	const int segmentHeight;
	const int redundantBorderSize;
	const int numberOfVerticalSlices;
	const int numberOfHorizontalSlices;
	const size_t capacity;
	GLuint* textures;

	GLuint vao;
	GLuint vbo_positions;
	GLuint vbo_texcoords;

	static int CalculateNumberOfSlices(int textureSize, int segmentSize, int redundantBorderSize)
	{
		int slices = (textureSize + redundantBorderSize) / (segmentSize - redundantBorderSize);
		if ((textureSize + redundantBorderSize) % (segmentSize - redundantBorderSize) != 0)
			++slices;
		return slices;
	}

	inline int GetTextureIndex(int column, int row) const
	{
		return column + row * numberOfVerticalSlices;
	}

	void UpdateTexture(int index, const SubImage& sub)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[index]);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		GLint internalFormat;
		GLenum format;

		switch (sub.componentsPerPixel) {
		case 4:
			internalFormat = GL_RGBA8;
			format = GL_RGBA;
			break;
		case 3:
			internalFormat = GL_RGB8;
			format = GL_RGB;
			break;
		case 1:
			internalFormat = GL_R8;
			format = GL_RED;
			break;
		default:
			throw std::runtime_error("Unsupported image format");
		}

		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat,
			sub.width, sub.height, 0, format, GL_UNSIGNED_BYTE, sub.data);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	void Bind(int index)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[index]);
	}

public:
	TextureCollection(
		const uint8_t* srcData,
		int imgWidth, int imgHeight, int imgCpp,
		int segmentWidth, int segmentHeight, int redundantBorderSize)
		: width(imgWidth),
		  height(imgHeight),
		  segmentWidth(segmentWidth),
		  segmentHeight(segmentHeight),
		  redundantBorderSize(redundantBorderSize),
		  numberOfVerticalSlices(CalculateNumberOfSlices(imgWidth, segmentWidth, redundantBorderSize)),
		  numberOfHorizontalSlices(CalculateNumberOfSlices(imgHeight, segmentHeight, redundantBorderSize)),
		  capacity((size_t)numberOfHorizontalSlices * numberOfVerticalSlices),
		  textures(new GLuint[capacity]),
		  vao(0), vbo_positions(0), vbo_texcoords(0)
	{
		glGenTextures(capacity, textures);
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo_positions);
		glGenBuffers(1, &vbo_texcoords);
		glBindVertexArray(vao);

		// Unit quad in object space: two triangles covering (0,0)–(1,1)
		float positions[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
		glBindBuffer(GL_ARRAY_BUFFER, vbo_positions);
		glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(0);

		float texCoords[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
		glBindBuffer(GL_ARRAY_BUFFER, vbo_texcoords);
		glBufferData(GL_ARRAY_BUFFER, sizeof(texCoords), texCoords, GL_STATIC_DRAW);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glBindVertexArray(0);

		// Producer-consumer: subsections are created on a worker thread and
		// uploaded to GPU on the calling (main) thread that owns the GL context.
		struct QueueItem { int index; SubImage* sub; };
		std::queue<QueueItem> queue;
		std::mutex mtx;
		std::condition_variable cv;
		std::atomic<size_t> consumed{0};

		std::thread producer([&]() {
			for (int y = 0; y < numberOfHorizontalSlices; ++y) {
				for (int x = 0; x < numberOfVerticalSlices; ++x) {
					int idx = GetTextureIndex(x, y);
					SubImage* sub = MakeSubsection(
						srcData, imgWidth, imgHeight, imgCpp,
						x * (segmentWidth - redundantBorderSize),
						y * (segmentHeight - redundantBorderSize),
						segmentWidth, segmentHeight);
					{
						std::lock_guard<std::mutex> lock(mtx);
						queue.push({idx, sub});
					}
					cv.notify_one();
				}
			}
		});

		while (consumed < capacity) {
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait(lock, [&] { return !queue.empty(); });
			auto item = queue.front();
			queue.pop();
			lock.unlock();
			UpdateTexture(item.index, *item.sub);
			delete item.sub;
			++consumed;
		}
		producer.join();
	}

	TextureCollection(TextureCollection&& other) noexcept
		: width(other.width),
		  height(other.height),
		  segmentWidth(other.segmentWidth),
		  segmentHeight(other.segmentHeight),
		  redundantBorderSize(other.redundantBorderSize),
		  numberOfVerticalSlices(other.numberOfVerticalSlices),
		  numberOfHorizontalSlices(other.numberOfHorizontalSlices),
		  capacity(other.capacity),
		  textures(other.textures),
		  vao(other.vao),
		  vbo_positions(other.vbo_positions),
		  vbo_texcoords(other.vbo_texcoords)
	{
		// Nullify other's resources to prevent double-free after move
		other.textures     = nullptr;
		other.vao = 0;
		other.vbo_positions = 0;
		other.vbo_texcoords = 0;
	}

	TextureCollection(const TextureCollection&) = delete;

	~TextureCollection()
	{
		if (textures) {
			glDeleteTextures(capacity, textures);
			delete[] textures;
		}
		if (vao) glDeleteVertexArrays(1, &vao);
		if (vbo_positions) glDeleteBuffers(1, &vbo_positions);
		if (vbo_texcoords) glDeleteBuffers(1, &vbo_texcoords);
	}

	void Draw(GLint modelLoc)
	{
		glBindVertexArray(vao);
		for (int y = 0; y < numberOfHorizontalSlices; ++y) {
			int hBorderTop    = (y != 0)                         ? redundantBorderSize / 2 : 0;
			int hBorderBottom = (y != numberOfHorizontalSlices - 1) ? redundantBorderSize / 2 : 0;
			int top_px    = y * (segmentHeight - redundantBorderSize) + hBorderTop;
			int bottom_px = y * (segmentHeight - redundantBorderSize) + segmentHeight - hBorderBottom;

			for (int x = 0; x < numberOfVerticalSlices; ++x) {
				int vBorderLeft  = (x != 0)                        ? redundantBorderSize / 2 : 0;
				int vBorderRight = (x != numberOfVerticalSlices - 1) ? redundantBorderSize / 2 : 0;
				int left_px  = x * (segmentWidth - redundantBorderSize) + vBorderLeft;
				int right_px = x * (segmentWidth - redundantBorderSize) + segmentWidth - vBorderRight;

				Bind(GetTextureIndex(x, y));

				float model[16] = {
					(float)(right_px - left_px), 0.0f, 0.0f, 0.0f,
					0.0f, (float)(bottom_px - top_px), 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					(float)left_px, (float)top_px, 0.0f, 1.0f
				};
				glUniformMatrix4fv(modelLoc, 1, GL_FALSE, model);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
		}
		glBindVertexArray(0);
		glUseProgram(0);
	}
};

// ---------------------------------------------------------------------------
// Renderer state
// ---------------------------------------------------------------------------

struct OpenGLRenderer {
	GLFWwindow* glfwWindow;
	GLuint shaderProgram;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Exported C interface
// ---------------------------------------------------------------------------

export extern "C" RENDERERPLUGIN_API void Renderer_SetWindowHints()
{
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
}

export extern "C" RENDERERPLUGIN_API RendererHandle Renderer_Create(void* glfwWindow)
{
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		throw std::runtime_error("gladLoadGLLoader failed");

	glEnable(GL_TEXTURE_2D);

	auto* renderer = new OpenGLRenderer{
		.glfwWindow = static_cast<GLFWwindow*>(glfwWindow),
		.shaderProgram = BuildDefaultShaderProgram()
	};
	return static_cast<RendererHandle>(renderer);
}

export extern "C" RENDERERPLUGIN_API void Renderer_Destroy(RendererHandle rendererHandle)
{
	if (!rendererHandle) return;
	auto* renderer = static_cast<OpenGLRenderer*>(rendererHandle);
	if (renderer->shaderProgram) glDeleteProgram(renderer->shaderProgram);
	delete renderer;
}

export extern "C" RENDERERPLUGIN_API void Renderer_Resize(
	RendererHandle /*rendererHandle*/, int width, int height)
{
	glViewport(0, 0, width, height);
}

export extern "C" RENDERERPLUGIN_API TextureCollectionHandle Renderer_LoadTextures(
	RendererHandle /*rendererHandle*/,
	const uint8_t* data,
	int width, int height, int componentsPerPixel,
	int segmentWidth, int segmentHeight, int redundantBorderSize)
{
	auto* tc = new TextureCollection(
		data, width, height, componentsPerPixel,
		segmentWidth, segmentHeight, redundantBorderSize);
	return static_cast<TextureCollectionHandle>(tc);
}

export extern "C" RENDERERPLUGIN_API void Renderer_FreeTextures(
	RendererHandle /*rendererHandle*/,
	TextureCollectionHandle textureHandle)
{
	delete static_cast<TextureCollection*>(textureHandle);
}

export extern "C" RENDERERPLUGIN_API void Renderer_Draw(
	RendererHandle rendererHandle,
	TextureCollectionHandle textureHandle,
	int winWidth, int winHeight,
	double zoom, double panX, double panY)
{
	if (!rendererHandle) return;
	auto* renderer = static_cast<OpenGLRenderer*>(rendererHandle);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(renderer->shaderProgram);

	GLint projectionLoc    = glGetUniformLocation(renderer->shaderProgram, "projection");
	GLint modelLoc         = glGetUniformLocation(renderer->shaderProgram, "model");
	GLint textureSamplerLoc = glGetUniformLocation(renderer->shaderProgram, "ourTexture");
	glUniform1i(textureSamplerLoc, 0);

	if (textureHandle != nullptr) {
		auto* tc = static_cast<TextureCollection*>(textureHandle);

		float imgW = (float)(tc->width  * zoom);
		float imgH = (float)(tc->height * zoom);
		float cx = (winWidth  - imgW) * 0.5f;
		float cy = (winHeight - imgH) * 0.5f;
		float left   = (float)((-cx - panX) / zoom);
		float right  = (float)((winWidth  - cx - panX) / zoom);
		float top    = (float)((-cy - panY) / zoom);
		float bottom = (float)((winHeight - cy - panY) / zoom);
		float nearVal = -1.0f, farVal = 1.0f;

		float ortho[16] = {
			2.0f / (right - left), 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
			0.0f, 0.0f, -2.0f / (farVal - nearVal), 0.0f,
			-(right + left) / (right - left),
			-(top + bottom) / (top - bottom),
			-(farVal + nearVal) / (farVal - nearVal),
			1.0f
		};
		glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, ortho);
		tc->Draw(modelLoc);
	}

	glfwSwapBuffers(renderer->glfwWindow);
}
