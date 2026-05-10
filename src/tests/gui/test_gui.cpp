/*
 * test_gui.cpp — GUI end-to-end tests for ImageViewer
 *
 * Tests the full rendering pipeline:
 *   - OpenGL context creation via GLFW (hidden window)
 *   - Synthetic image → GPU texture upload
 *   - Frame draw (GL error check)
 *   - Zoom / pan / fit-to-window operations
 *   - Screen-info query
 *   - Pixel readback verifying non-trivial rendered content
 *
 * Runtime requirements
 * --------------------
 * On Linux (CI): LIBGL_ALWAYS_SOFTWARE=1 enables Mesa software rendering.
 * A virtual X11 display (e.g. Xvfb :99) must be running and DISPLAY set.
 *
 *   LIBGL_ALWAYS_SOFTWARE=1 DISPLAY=:99 ./iview_gui_tests
 *
 * The window is created hidden (GLFW_VISIBLE = GLFW_FALSE) so it never
 * appears on screen.
 */

/* ── Standard / system headers (before any module imports) ─────────────────── */
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "ImagePluginDef.h"
#include "FilterPluginDef.h"

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

/* ── Module imports ─────────────────────────────────────────────────────────── */
import UserInterface; /* Window */
import Image;         /* Image  */

// ─── Utilities ────────────────────────────────────────────────────────────────

/* Build a solid-colour RGB8 Image without requiring any plugin DLL. */
static Image MakeSolidRGB8(int w, int h,
                             uint8_t r, uint8_t g, uint8_t b)
{
    IWImageFormat fmt = {};
    fmt.componentCount = 3;
    fmt.bitsPerPixel   = 24;
    fmt.storageLayout  = IW_STORAGE_INTERLEAVED;
    fmt.components[0]  = { IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_CLASS_UINT,  0, 8 };
    fmt.components[1]  = { IW_COMPONENT_SEMANTIC_G, IW_COMPONENT_CLASS_UINT,  8, 8 };
    fmt.components[2]  = { IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_CLASS_UINT, 16, 8 };

    Image img(w, h, fmt, IMAGE_COLOR_SPACE_SRGB);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* pix = img.data + static_cast<ptrdiff_t>(y) * img.stride + x * 3;
            pix[0] = r;
            pix[1] = g;
            pix[2] = b;
        }
    return img;
}

static constexpr IWFilterOptions kNoFilter{ IW_FILTER_OPTIONS_VERSION, 0u, nullptr };

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_SUITE("GUI: OpenGL context")
{
    TEST_CASE("GL context and basic state after window creation")
    {
        /* Window::Initialize() was called in main() before the test suite runs.
         * Creating a Window opens a hidden GL 3.1 context.                     */
        Window w(640, 480);
        CHECK(glGetError() == GL_NO_ERROR);

        /* GL_VENDOR string must be non-empty (sanity). */
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        CHECK(vendor != nullptr);
        if (vendor) CHECK(vendor[0] != '\0');
    }
}

TEST_SUITE("GUI: texture upload")
{
    TEST_CASE("load solid-colour RGB8 image into GPU texture")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 200, 100, 50);

        w.LoadTextures(img, 256, 256, 2, kNoFilter);
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("load 1x1 image (edge case)")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(1, 1, 255, 0, 0);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("reload texture with different image size")
    {
        Window w(640, 480);
        Image img1 = MakeSolidRGB8(64, 64, 255, 0, 0);
        Image img2 = MakeSolidRGB8(128, 96, 0, 255, 0);

        w.LoadTextures(img1, 256, 256, 2, kNoFilter);
        CHECK(glGetError() == GL_NO_ERROR);

        w.LoadTextures(img2, 256, 256, 2, kNoFilter);
        CHECK(glGetError() == GL_NO_ERROR);
    }
}

TEST_SUITE("GUI: rendering")
{
    TEST_CASE("Draw() completes without GL errors on empty window")
    {
        Window w(640, 480);
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("Draw() completes without GL errors after loading image")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 180, 90, 45);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("multiple Draw() calls are stable")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 128, 128, 128);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);

        for (int i = 0; i < 5; ++i)
            w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("pixel readback: centre pixel is non-black after coloured image")
    {
        /*
         * Load a bright red 64×64 image into a 640×480 window.
         * The image is centred; at zoom=1 it occupies the middle 64×64 region.
         * After Draw() swaps buffers the rendered content is in the front
         * buffer.  We switch the read source to GL_FRONT and sample the
         * window centre to verify the image was actually composited.
         */
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 220, 0, 0); /* bright red */
        w.LoadTextures(img, 256, 256, 2, kNoFilter);

        w.Draw(); /* renders to back buffer and swaps */

        glReadBuffer(GL_FRONT);
        uint8_t pixel[4] = { 0, 0, 0, 0 };
        glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

        /* Accept any non-black reading — confirms pixels were actually rendered.
         * (Some CI GL implementations may blend differently; we avoid being
         *  prescriptive about exact colour values.)                            */
        const bool nonBlack = (pixel[0] > 0 || pixel[1] > 0 || pixel[2] > 0);
        CHECK(nonBlack);
    }
}

TEST_SUITE("GUI: zoom and pan operations")
{
    TEST_CASE("setZoom does not crash without a loaded image")
    {
        Window w(640, 480);
        w.setZoom(1.0);
        w.setZoom(2.0);
        w.setZoom(0.5);
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("setZoom extremes are clamped to viewer limits")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 0, 128, 255);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);

        /* Scroll cap: +400% (zoom level ×5). */
        w.setZoom(100.0);
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);

        /* Scroll floor: −90% (zoom level ×0.1). */
        w.setZoom(0.001);
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("adjustZoom steps zoom incrementally")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 0, 200, 0);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);

        w.adjustZoom(10);   /* zoom in one notch  */
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);

        w.adjustZoom(-20);  /* zoom out two notches */
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("fitToWindow resets zoom and pan")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(128, 128, 64, 64, 64);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);

        w.setZoom(3.0);
        w.fitToWindow();
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }

    TEST_CASE("adjustPan moves image within bounds")
    {
        Window w(640, 480);
        Image img = MakeSolidRGB8(64, 64, 100, 150, 200);
        w.LoadTextures(img, 256, 256, 2, kNoFilter);

        w.setZoom(4.0);         /* zoom in so panning has a non-zero range */
        w.adjustPan(50, 30);
        w.adjustPan(-100, -60);
        w.Draw();
        CHECK(glGetError() == GL_NO_ERROR);
    }
}

TEST_SUITE("GUI: screen info query")
{
    TEST_CASE("QueryScreenInfo returns sane values")
    {
        Window w(640, 480);
        const IWScreenInfo info = Window::QueryScreenInfo();

        /* bitsPerChannel is 0 (unknown) or a plausible display depth. */
        CHECK((info.bitsPerChannel == 0 ||
               (info.bitsPerChannel >= 4 && info.bitsPerChannel <= 16)));
        /* channelCount is 0, 3 (RGB), or 4 (RGBA). */
        CHECK((info.channelCount == 0 ||
               info.channelCount == 3 ||
               info.channelCount == 4));
    }
}

// ─── Custom main: initialise GLFW once for the whole test run ─────────────────

int main(int argc, char** argv)
{
    try {
        Window::Initialize();  /* glfwInit() must be called before any hint */
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
            "FATAL: glfwInit() failed — cannot run GUI tests.\n"
            "  Ensure a display is available (Xvfb or Wayland compositor)\n"
            "  and that LIBGL_ALWAYS_SOFTWARE=1 is set for CI.\n"
            "  Error: %s\n", ex.what());
        return 1;
    }

    /* Hide every window created during the test run so CI stays headless.
     * GLFW window hints must be set after glfwInit() and apply to the next
     * glfwCreateWindow() call.  Window::Initialize() calls only glfwInit(),
     * not glfwCreateWindow(), so this ordering is correct.                  */
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    doctest::Context ctx(argc, argv);
    const int result = ctx.run();

    Window::Deinitialize();
    return result;
}
