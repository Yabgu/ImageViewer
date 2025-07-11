module;

#include <stdexcept>
#include <thread>
#include <vector>
#include <glad/glad.h>

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
		const auto end = tc.numberOfVerticalSlices * tc.numberOfHorizontalSlices;
		for (auto y = 0; y < tc.numberOfHorizontalSlices; ++y)
		{
			for (auto x = 0; x < tc.numberOfVerticalSlices; ++x)
			{
				auto i = tc.GetTextureIndex(x, y);
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
		capacity((size_t)numberOfHorizontalSlices* numberOfVerticalSlices),
		textures(new GLuint[capacity])
	{
		glGenTextures(capacity, textures);

		std::vector<Image*> subsection(capacity);
		auto toBeProcessed = capacity;

		auto start_time = std::chrono::high_resolution_clock::now();
#pragma omp parallel num_threads(2)
		{
#pragma omp master
			{
#pragma omp task
				{
					MakeSubImages(image, subsection, *this);
				}

				while (toBeProcessed)
				{
					for (auto i=0; i < capacity; ++i)
					{
						auto csubsection = subsection[i];
						subsection[i] = nullptr;
						if (csubsection != nullptr)
						{
							-- toBeProcessed;
							UpdateTexture(i, *csubsection);
							delete csubsection;
						}
					}
					std::this_thread::yield();
				}
			}
		}
	}

	TextureCollection(TextureCollection&&) = default;
	TextureCollection(const TextureCollection&) = delete;

	~TextureCollection()
	{
		glDeleteTextures(capacity, textures);
		delete[] textures;
	}

	void Bind(int index)
	{
		if (index < 0)
		{
			[[unlikely]] throw std::runtime_error("negative index");
		}
		glBindTexture(GL_TEXTURE_2D, textures[index]);
	}

	GLuint UpdateTexture(const int index, const Image& image)
	{
		if (index >= capacity)
		{
			[[unlikely]] throw std::runtime_error("Max capacity reached for texture pool");
		}

		glBindTexture(GL_TEXTURE_2D, textures[index]);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		switch (image.componentsPerPixel)
		{
		case 4:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data);
			break;
		case 3:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, image.width, image.height, 0, GL_RGB, GL_UNSIGNED_BYTE, image.data);
			break;
		case 1:
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, image.width, image.height, 0, GL_RED, GL_UNSIGNED_BYTE, image.data);
			break;
		default:
			[[unlikely]] throw std::runtime_error("Unsupported image format");
		}

		return textures[index];
	}

	void Draw()
	{
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		glScalef(1.0 / segmentWidth, 1.0 / segmentHeight, 1.0);

		glMatrixMode(GL_MODELVIEW);
		for (auto y = 0; y < numberOfHorizontalSlices; ++y)
		{
			int horizontalBorderTop = redundantBorderSize / 2 * (y != 0);
			int horizontalBorderBottom = redundantBorderSize / 2 * (y != numberOfHorizontalSlices - 1);
			int top = y * (segmentHeight - redundantBorderSize) + horizontalBorderTop;
			int bottom = y * (segmentHeight - redundantBorderSize) + segmentHeight - horizontalBorderBottom;
			int textureTop = horizontalBorderTop;
			int textureBottom = segmentWidth - horizontalBorderBottom;

			for (auto x = 0; x < numberOfVerticalSlices; ++x)
			{
				int verticalBorderLeft = redundantBorderSize / 2 * (x != 0);
				int verticalBorderRight = redundantBorderSize / 2 * (x != numberOfVerticalSlices - 1);
				int left = x * (segmentWidth - redundantBorderSize) + verticalBorderLeft;
				int right = x * (segmentWidth - redundantBorderSize) + segmentWidth - verticalBorderRight;
				int textureLeft = verticalBorderLeft;
				int textureRight = segmentWidth - verticalBorderRight;

				Bind(x + y * numberOfVerticalSlices);
				glBegin(GL_TRIANGLE_STRIP);
				glTexCoord2i(textureLeft, textureBottom);
				glVertex2i(left, bottom);
				glTexCoord2i(textureRight, textureBottom);
				glVertex2i(right, bottom);
				glTexCoord2i(textureLeft, textureTop);
				glVertex2i(left, top);
				glTexCoord2i(textureRight, textureTop);
				glVertex2i(right, top);
				glEnd();
			}
		}
	}
};