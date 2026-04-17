#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Export macro for cross-platform symbol visibility
#ifndef RENDERERPLUGIN_API
#ifdef _WIN32
#define RENDERERPLUGIN_API __declspec(dllexport)
#else
#define RENDERERPLUGIN_API __attribute__((visibility("default")))
#endif
#endif

// Opaque handles used across the plugin boundary
typedef void* RendererHandle;
typedef void* TextureCollectionHandle;

// Set renderer-specific GLFW window hints before glfwCreateWindow is called
typedef void (*Renderer_SetWindowHintsFunc)();

// Initialize the renderer using an already-current GLFW/OpenGL context.
// Returns an opaque RendererHandle that must be passed to every subsequent call.
typedef RendererHandle (*Renderer_CreateFunc)(void* glfwWindow);

// Tear down the renderer and free all resources it owns.
typedef void (*Renderer_DestroyFunc)(RendererHandle renderer);

// Notify the renderer that the window has been resized (e.g. to update the viewport).
typedef void (*Renderer_ResizeFunc)(RendererHandle renderer, int width, int height);

// Upload raw RGBA/RGB/R pixel data as a tiled texture collection.
// Returns an opaque TextureCollectionHandle (null on failure).
typedef TextureCollectionHandle (*Renderer_LoadTexturesFunc)(
    RendererHandle renderer,
    const uint8_t* data,
    int width, int height, int componentsPerPixel,
    int segmentWidth, int segmentHeight, int redundantBorderSize);

// Free a texture collection previously returned by Renderer_LoadTextures.
typedef void (*Renderer_FreeTexturesFunc)(
    RendererHandle renderer,
    TextureCollectionHandle textures);

// Render one frame and present it.
// textures may be null, in which case the renderer clears to background and swaps.
typedef void (*Renderer_DrawFunc)(
    RendererHandle renderer,
    TextureCollectionHandle textures,
    int winWidth, int winHeight,
    double zoom, double panX, double panY);

#ifdef __cplusplus
}
#endif
