#include <glad/glad.h>
#include <stdexcept>

import Image;
export module TexturePool;

struct GLTexture
{
	GLuint texture;
	int textureWidth;
	int textureHeight;
	int x;
	int y;
	int width;
	int height;
};

export struct GLTexturePool
{
private:
	size_t size;
	size_t capacity;
	GLuint* textures;

public:
	GLTexturePool(size_t capacity)
		: size(0), capacity(capacity), textures(new GLuint[capacity])
	{
		glGenTextures(capacity, textures);
	}
	
	GLTexturePool(GLTexturePool&&) = default;
	GLTexturePool(const GLTexturePool&) = delete;

	~GLTexturePool()
	{
		glDeleteTextures(capacity, textures);
		delete[] textures;
	}

	void Bind(int index)
	{
		if (index < 0)
		{
			[[unlikely]]
			throw std::runtime_error("negative index");
		}
		glBindTexture(GL_TEXTURE_2D, textures[index]);
	}

	GLuint AddTexture(const Image& image)
	{
		if (size >= capacity)
		{
			[[unlikely]]
			throw std::runtime_error("Max capacity reached for texture pool");
		}

		glBindTexture(GL_TEXTURE_2D, textures[size]);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);	
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image.width, image.height, 0, GL_RGB, GL_UNSIGNED_BYTE, image.data);
		return textures[size++];
	}
};