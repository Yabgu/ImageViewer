module;

#include <cstdint>
#include <cstring>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <glad/glad.h>
#include "ImagePluginDef.h"
#include "Utils.hpp"

export module TexturePool;

import Image;

// ─── GL parameter inference ───────────────────────────────────────────────────

struct GLTexParams {
    GLenum internalFormat;
    GLenum format;
    GLenum type;
    bool   valid;
};

/* Try to map an IWImageFormat to a direct glTexImage2D call.
 * Only interleaved, uniform-width, byte-aligned, standard-semantic formats
 * with 8-bit uint, 16-bit uint, or 32-bit float components are accepted.
 * Returns valid=false for anything else (caller falls back to RGBA F32). */
static GLTexParams TryGetGLParams(const IWImageFormat& fmt) noexcept
{
    constexpr GLTexParams INVALID = {0, 0, 0, false};

    if (fmt.storageLayout != IW_STORAGE_INTERLEAVED) return INVALID;

    const uint16_t n = fmt.componentCount;
    if (n == 0u || n > 4u) return INVALID;

    const IWComponentClass cls = fmt.components[0].componentClass;
    const uint16_t         bw  = fmt.components[0].bitWidth;
    for (uint16_t i = 1u; i < n; ++i)
        if (fmt.components[i].componentClass != cls || fmt.components[i].bitWidth != bw)
            return INVALID;

    const bool isFloat = (cls == IW_COMPONENT_CLASS_FLOAT);
    GLenum glType;
    if      (isFloat  && bw == 32u) glType = GL_FLOAT;
    else if (!isFloat && bw ==  8u) glType = GL_UNSIGNED_BYTE;
    else if (!isFloat && bw == 16u) glType = GL_UNSIGNED_SHORT;
    else return INVALID;   /* float16, float64, 1-bit, etc. need conversion */

    auto s = [&](int i) { return fmt.components[i].semantic; };

    GLenum glFmt, intFmt;
    if (n == 1u && s(0) == IW_COMPONENT_SEMANTIC_GRAY) {
        glFmt  = GL_RED;
        intFmt = isFloat ? GL_R32F : (bw == 16u ? GL_R16  : GL_R8);
    } else if (n == 2u && s(0) == IW_COMPONENT_SEMANTIC_GRAY &&
                          s(1) == IW_COMPONENT_SEMANTIC_A) {
        glFmt  = GL_RG;
        intFmt = isFloat ? GL_RG32F : (bw == 16u ? GL_RG16 : GL_RG8);
    } else if (n == 3u && s(0) == IW_COMPONENT_SEMANTIC_R &&
                          s(1) == IW_COMPONENT_SEMANTIC_G &&
                          s(2) == IW_COMPONENT_SEMANTIC_B) {
        glFmt  = GL_RGB;
        intFmt = isFloat ? GL_RGB32F : (bw == 16u ? GL_RGB16 : GL_RGB8);
    } else if (n == 3u && s(0) == IW_COMPONENT_SEMANTIC_B &&
                          s(1) == IW_COMPONENT_SEMANTIC_G &&
                          s(2) == IW_COMPONENT_SEMANTIC_R) {
        glFmt  = GL_BGR;
        intFmt = isFloat ? GL_RGB32F : (bw == 16u ? GL_RGB16 : GL_RGB8);
    } else if (n == 4u && s(0) == IW_COMPONENT_SEMANTIC_R &&
                          s(1) == IW_COMPONENT_SEMANTIC_G &&
                          s(2) == IW_COMPONENT_SEMANTIC_B &&
                          s(3) == IW_COMPONENT_SEMANTIC_A) {
        glFmt  = GL_RGBA;
        intFmt = isFloat ? GL_RGBA32F : (bw == 16u ? GL_RGBA16 : GL_RGBA8);
    } else if (n == 4u && s(0) == IW_COMPONENT_SEMANTIC_B &&
                          s(1) == IW_COMPONENT_SEMANTIC_G &&
                          s(2) == IW_COMPONENT_SEMANTIC_R &&
                          s(3) == IW_COMPONENT_SEMANTIC_A) {
        glFmt  = GL_BGRA;
        intFmt = isFloat ? GL_RGBA32F : (bw == 16u ? GL_RGBA16 : GL_RGBA8);
    } else {
        return INVALID;   /* non-RGB/RGBA semantics (H, S, L, UV, …) */
    }

    return {intFmt, glFmt, glType, true};
}

/* Convert any interleaved IWImageFormat to RGBA F32.
 * Returns a heap-allocated buffer of width*height*4*sizeof(float) bytes;
 * the caller must delete[] it. */
static float* ConvertToRGBAF32(const Image& img)
{
    const IWImageFormat& fmt = img.format;
    const int            bpp = img.BytesPerPixel();
    const int            n   = img.ComponentCount();
    float* dst = new float[static_cast<size_t>(img.width) * img.height * 4u];

    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.data + static_cast<ptrdiff_t>(y) * img.stride;
        float*         out = dst + static_cast<ptrdiff_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            const uint8_t* pixel = row + x * bpp;
            float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            int genericSlot = 0; /* maps H/S/L/UV/… sequentially to R/G/B */
            for (int c = 0; c < n; ++c) {
                const float v = ExtractComponent(pixel, fmt.components[c]);
                switch (fmt.components[c].semantic) {
                case IW_COMPONENT_SEMANTIC_NONE:                    break; /* unused slot – skip */
                case IW_COMPONENT_SEMANTIC_R:    rgba[0] = v;       break;
                case IW_COMPONENT_SEMANTIC_G:    rgba[1] = v;       break;
                case IW_COMPONENT_SEMANTIC_B:    rgba[2] = v;       break;
                case IW_COMPONENT_SEMANTIC_A:    rgba[3] = v;       break;
                case IW_COMPONENT_SEMANTIC_GRAY: rgba[0] = rgba[1] = rgba[2] = v; break;
                default:
                    if (genericSlot < 3) rgba[genericSlot] = v;
                    ++genericSlot;
                    break;
                }
            }
            out[x * 4 + 0] = rgba[0];
            out[x * 4 + 1] = rgba[1];
            out[x * 4 + 2] = rgba[2];
            out[x * 4 + 3] = rgba[3];
        }
    }
    return dst;
}

export struct TextureCollection
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
	GLuint* const textures;

	// OpenGL 4.x specific additions for rendering
	GLuint vao;
	GLuint vbo_positions;
	GLuint vbo_texcoords;

	static int CalculateNumberOfSlices(const int textureSize, const int segmentSize, const int redundantBorderSize)
	{
		// Calculate slices
		int slices = (textureSize + redundantBorderSize) / (segmentSize - redundantBorderSize);
		if ((textureSize + redundantBorderSize) % (segmentSize - redundantBorderSize) != 0)
		{
			++slices;
		}
		return slices;
	}

	inline int GetTextureIndex(int column, int row) const
	{
		return column + row * numberOfVerticalSlices;
	}

	static void MakeSubImages(const Image& image, std::vector<Image*>& subsection, TextureCollection& tc)
	{
		for (auto y = 0; y < tc.numberOfHorizontalSlices; ++y)
		{
			for (auto x = 0; x < tc.numberOfVerticalSlices; ++x)
			{
				auto i = tc.GetTextureIndex(x, y);
				// The Subsection method should handle the redundantBorderSize logic for cropping correctly.
				auto section = image.Subsection(x * (tc.segmentWidth - tc.redundantBorderSize), y * (tc.segmentHeight - tc.redundantBorderSize), tc.segmentWidth, tc.segmentHeight);
				subsection[i] = section;
			}
		}
	}

public:
	TextureCollection(
		const Image& image,
		const int segmentWidth,
		const int segmentHeight,
		const int redundantBorderSize)
		: width(image.width),
		height(image.height),
		segmentWidth(segmentWidth),
		segmentHeight(segmentHeight),
		redundantBorderSize(redundantBorderSize),
		numberOfVerticalSlices(CalculateNumberOfSlices(width, segmentWidth, redundantBorderSize)),
		numberOfHorizontalSlices(CalculateNumberOfSlices(height, segmentHeight, redundantBorderSize)),
		capacity((size_t)numberOfHorizontalSlices * numberOfVerticalSlices),
		textures(new GLuint[capacity]),
		vao(0), vbo_positions(0), vbo_texcoords(0)
	{
		glGenTextures(capacity, textures);
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo_positions);
		glGenBuffers(1, &vbo_texcoords);
		glBindVertexArray(vao);
		float positions[] = {
			0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f
		};
		glBindBuffer(GL_ARRAY_BUFFER, vbo_positions);
		glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(0);
		float texCoords[] = {
			0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f
		};
		glBindBuffer(GL_ARRAY_BUFFER, vbo_texcoords);
		glBufferData(GL_ARRAY_BUFFER, sizeof(texCoords), texCoords, GL_STATIC_DRAW);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glBindVertexArray(0);

		// Thread-safe queue for producer-consumer
		struct QueueItem {
			int index;
			Image* img;
		};
		std::queue<QueueItem> queue;
		std::mutex mtx;
		std::condition_variable cv;
		std::atomic<size_t> produced{0};
		std::atomic<size_t> consumed{0};

		// Producer: create subsections in parallel
		std::thread producer([&]() {
			for (int y = 0; y < numberOfHorizontalSlices; ++y) {
				for (int x = 0; x < numberOfVerticalSlices; ++x) {
					int idx = GetTextureIndex(x, y);
					Image* sub = image.Subsection(x * (segmentWidth - redundantBorderSize), y * (segmentHeight - redundantBorderSize), segmentWidth, segmentHeight);
					{
						std::lock_guard<std::mutex> lock(mtx);
						queue.push({idx, sub});
					}
					cv.notify_one();
					++produced;
				}
			}
		});

		// Consumer: main thread uploads to GPU
		while (consumed < capacity) {
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait(lock, [&] { return !queue.empty(); });
			auto item = queue.front();
			queue.pop();
			lock.unlock();
			UpdateTexture(item.index, *item.img);
			delete item.img;
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
		// Nullify other's pointers to prevent double deletion
		const_cast<GLuint*&>(other.textures) = nullptr;
		other.vao = 0;
		other.vbo_positions = 0;
		other.vbo_texcoords = 0;
	}

	TextureCollection(const TextureCollection&) = delete;

	~TextureCollection()
	{
		if (textures) { // Check if textures array was allocated and not moved
			glDeleteTextures(capacity, textures);
			delete[] textures;
		}
		if (vao) glDeleteVertexArrays(1, &vao);
		if (vbo_positions) glDeleteBuffers(1, &vbo_positions);
		if (vbo_texcoords) glDeleteBuffers(1, &vbo_texcoords);
	}

	void Bind(int index)
	{
		if (index < 0 || index >= capacity)
		{
			[[unlikely]] throw std::runtime_error("Index out of bounds for texture binding");
		}
		// In OpenGL 4.x, you typically activate a texture unit first.
		// For simple single-texture drawing, GL_TEXTURE0 is fine.
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[index]);
	}

	GLuint UpdateTexture(const int index, const Image& image)
	{
		if (index >= capacity)
			[[unlikely]] throw std::runtime_error("Max capacity reached for texture pool");

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[index]);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		const GLTexParams gp = TryGetGLParams(image.format);
		if (gp.valid) {
			// Fast path: format is directly GL-uploadable.
			glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(gp.internalFormat),
			             image.width, image.height, 0,
			             gp.format, gp.type, image.data);
		} else {
			// Slow path: convert component-by-component to RGBA F32, then upload.
			float* tmp = ConvertToRGBAF32(image);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
			             image.width, image.height, 0,
			             GL_RGBA, GL_FLOAT, tmp);
			delete[] tmp;
		}

		glBindTexture(GL_TEXTURE_2D, 0);
		return textures[index];
	}

	void Draw(GLint modelLoc) // Only takes modelLoc now
	{
		glBindVertexArray(vao);
		for (auto y = 0; y < numberOfHorizontalSlices; ++y)
		{
			int horizontalBorderTop = redundantBorderSize / 2 * (y != 0);
			int horizontalBorderBottom = redundantBorderSize / 2 * (y != numberOfHorizontalSlices - 1);
			int top_px = y * (segmentHeight - redundantBorderSize) + horizontalBorderTop;
			int bottom_px = y * (segmentHeight - redundantBorderSize) + segmentHeight - horizontalBorderBottom;

			for (auto x = 0; x < numberOfVerticalSlices; ++x)
			{
				int verticalBorderLeft = redundantBorderSize / 2 * (x != 0);
				int verticalBorderRight = redundantBorderSize / 2 * (x != numberOfVerticalSlices - 1);
				int left_px = x * (segmentWidth - redundantBorderSize) + verticalBorderLeft;
				int right_px = x * (segmentWidth - redundantBorderSize) + segmentWidth - verticalBorderRight;

				Bind(GetTextureIndex(x, y));

				// Model matrix: identity, translate to segment position, scale to segment size
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