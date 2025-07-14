module;

#include <stdexcept>
#include <thread>
#include <glad/glad.h>
#include <queue>
#include <mutex>
#include <condition_variable>

import Image;
export module TexturePool;
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
		{
			[[unlikely]] throw std::runtime_error("Max capacity reached for texture pool");
		}

		// Bind the specific texture to configure it
		glActiveTexture(GL_TEXTURE0); // Activate a texture unit first
		glBindTexture(GL_TEXTURE_2D, textures[index]);

		// Using GL_CLAMP_TO_EDGE instead of GL_CLAMP
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		GLint internalFormat;
		GLenum format;

		switch (image.componentsPerPixel)
		{
		case 4:
			internalFormat = GL_RGBA8; // Use sized internal format for modern OpenGL
			format = GL_RGBA;
			break;
		case 3:
			internalFormat = GL_RGB8; // Use sized internal format
			format = GL_RGB;
			break;
		case 1:
			internalFormat = GL_R8; // Use sized internal format
			format = GL_RED;
			break;
		default:
			[[unlikely]] throw std::runtime_error("Unsupported image format");
		}

		// Use glTexImage2D for initial upload. For updates without changing format/size, glTexSubImage2D is an alternative.
		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, image.width, image.height, 0, format, GL_UNSIGNED_BYTE, image.data);
		// Optionally generate mipmaps if you plan to use them.
		// glGenerateMipmap(GL_TEXTURE_2D);

		// Unbind the texture to prevent accidental modification (good practice)
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