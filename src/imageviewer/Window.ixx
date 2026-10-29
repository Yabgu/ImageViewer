module;

#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <format>
#include <future>
#include <string>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <algorithm>
#include <utility>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Dear ImGui is pulled in through the global module fragment only: no ImGui
 * type appears in the exported interface below.  The backend is the Vulkan one,
 * and it is compiled with IMGUI_IMPL_VULKAN_USE_VOLK so it shares volk's
 * function pointers with this module (no second loader, no vulkan-1.lib). */
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "FilterPluginDef.h"
#include "FilterCore.hpp"

export module UserInterface;

import VulkanContext;
import VulkanRenderer;
import Image;
import HotkeysHandler;
import FilterPlugin;

/* All Dear ImGui state lives here.  The type is attached to this module and is
 * never exported, so the UserInterface interface stays free of ImGui types
 * (pimpl, see Window::imgui_). */
struct WindowImGuiState
{
	bool  initialized      = false;
	bool  chromeEnabled    = true;   /* Tab toggles this */
	bool  frameClockStarted = false;
	float alpha            = 0.0f;
	std::string readout;             /* "1920x1080 . 3.2 MB", filled once on upload */
	std::chrono::steady_clock::time_point lastMouseActivity{};
};

export class Window
{
protected:
	static Window* From(GLFWwindow* window) noexcept
	{
		return reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
	}

	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = From(window);
		if (win != nullptr)
		{
			win->width  = width;
			win->height = height;
			win->resizePending_ = true;
		}
	}

	static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
	{
		Window* win = From(window);
		if (win != nullptr)
		{
			/* The swapchain is created at framebuffer resolution, so this is the
			 * size that matters; the window keeps its own size in GLFW. */
			win->width  = width;
			win->height = height;
			win->resizePending_ = true;
		}
	}

	static void window_focus_callback(GLFWwindow* window, int focused)
	{
		Window* win = From(window);
		if (win != nullptr && focused == GLFW_TRUE) win->dirtyRequest_ = true;
	}

	static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
	{
		Window* win = From(window);
		/* Leave the wheel to the ImGui chrome when the cursor is over it. */
		if (win != nullptr && !ImguiWantsMouse())
		{
			win->applyScroll(xoffset, yoffset);
		}
		win->dirtyRequest_ = true;
	}

	/* GLFW file drop.  ImGui's GLFW backend does not install a drop callback, so
	 * this one survives ImGui_ImplGlfw_InitForVulkan(..., true) untouched. */
	static void drop_callback(GLFWwindow* window, int count, const char** paths)
	{
		Window* win = From(window);
		if (win != nullptr)
		{
			win->HandleDrop(count, paths);
			win->dirtyRequest_ = true;
		}
	}

	void applyScroll(double xoffset, double yoffset)
	{
		constexpr int MIN_ZOOM_PERCENT = -90;
		constexpr int MAX_ZOOM_PERCENT = 400;
		constexpr double SCROLL_SENSITIVITY = 10;

		double mouseX, mouseY;
		glfwGetCursorPos(glfwWindow, &mouseX, &mouseY);

		double newZoomLevel = (scroll + yoffset * SCROLL_SENSITIVITY + 100.0) / 100.0;
		newZoomLevel = std::clamp(newZoomLevel, (MIN_ZOOM_PERCENT + 100.0) / 100.0, (MAX_ZOOM_PERCENT + 100.0) / 100.0);

		setZoom(newZoomLevel, mouseX, mouseY);
	}

private:
	GLFWwindow* glfwWindow = nullptr;
	int width;
	int height;
	int scroll;
	int imageWidth_  = 0;
	int imageHeight_ = 0;
	FilterPlugin filterPlugin_;
	double panX = 0.0;
	double panY = 0.0;
	bool isPanning = false;
	double lastMouseX = 0.0, lastMouseY = 0.0;

	/* Dear ImGui private state (see WindowImGuiState at module scope). */
	std::unique_ptr<WindowImGuiState> imgui_;
	/* Owned copy of the filter options from the command line. */
	IWFilterOptions filterOptions_{ IW_FILTER_OPTIONS_VERSION, 0u, nullptr };
	IWFilterArg filterArgStorage_[5]{};

	/* Set by the GLFW callbacks; consumed by Main. */
	mutable bool dirtyRequest_  = false;
	bool         resizePending_ = false;

	VulkanContext  vk_;
	VulkanRenderer renderer_;
	VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;

	double getZoom() const {
		return (scroll + 100.0) / 100.0;
	}

	int getZoomPercent() const noexcept {
		return static_cast<int>(std::lround(getZoom() * 100.0));
	}

	bool HasImage() const noexcept { return imageWidth_ > 0 && imageHeight_ > 0; }

	/* True when the ImGui chrome currently owns the mouse (hovering a widget). */
	static bool ImguiWantsMouse() noexcept {
		if (ImGui::GetCurrentContext() == nullptr) return false;
		return ImGui::GetIO().WantCaptureMouse;
	}

	void setPanning(double x, double y) {
		if (HasImage()) {
			double zoom = getZoom();
			double imgW = imageWidth_ * zoom;
			double imgH = imageHeight_ * zoom;
			double minPanX = -(imgW - width) * 0.5;
			double maxPanX = (imgW - width) * 0.5;
			double minPanY = -(imgH - height) * 0.5;
			double maxPanY = (imgH - height) * 0.5;

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
		dirtyRequest_ = true;
	}

	static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
		Window* win = From(window);
		if (button == GLFW_MOUSE_BUTTON_LEFT) {
			if (action == GLFW_PRESS) {
				if (win == nullptr || ImguiWantsMouse()) return;
				double x, y;
				glfwGetCursorPos(window, &x, &y);
				win->isPanning = true;
				win->lastMouseX = x;
				win->lastMouseY = y;
			} else if (action == GLFW_RELEASE) {
				if (win != nullptr) win->isPanning = false;
			}
		}
		if (win != nullptr) win->dirtyRequest_ = true;
	}

	static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
		Window* win = From(window);
		if (win == nullptr) return;
		if (win->isPanning) {
			double dx = xpos - win->lastMouseX;
			double dy = ypos - win->lastMouseY;
			win->setPanning(win->panX + dx, win->panY + dy);
			win->lastMouseX = xpos;
			win->lastMouseY = ypos;
		}
		win->dirtyRequest_ = true;
	}

	static GLFWwindow* InitializeWindow(int width, int height, Window* containerWindow)
	{
		/* No client API: GLFW only provides the window and the Vulkan surface.
		 * The window stays invisible until the first frame that contains the
		 * decoded image has been presented (see Window::Main). */
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

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

		glfwSetWindowSizeCallback(glfwWindow, window_size_callback);
		glfwSetFramebufferSizeCallback(glfwWindow, framebuffer_size_callback);
		glfwSetWindowFocusCallback(glfwWindow, window_focus_callback);
		glfwSetScrollCallback(glfwWindow, scroll_callback);
		glfwSetMouseButtonCallback(glfwWindow, mouse_button_callback);
		glfwSetCursorPosCallback(glfwWindow, cursor_position_callback);
		glfwSetKeyCallback(glfwWindow, key_callback);
		glfwSetDropCallback(glfwWindow, drop_callback);

		return glfwWindow;
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
		  hotkeysHandler() {
		/* Diagnostics must interleave truthfully with the Vulkan validation
		 * layer's own stderr writes. */
		std::setvbuf(stderr, nullptr, _IONBF, 0);
		IviewTrace("window created");

		int fbWidth = width, fbHeight = height;
		glfwGetFramebufferSize(glfwWindow, &fbWidth, &fbHeight);

		/* Instance, surface, device and the first FIFO swapchain.  This runs on
		 * the main thread while the decode future is still working. */
		vk_.Initialize(glfwWindow, (uint32_t)fbWidth, (uint32_t)fbHeight);
		renderer_.Initialize(vk_);

		InitImGui();
		IviewTrace("ImGui initialized (vulkan backend)");

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

	~Window() = default;

private:
	void InitImGui()
	{
		imgui_ = std::make_unique<WindowImGuiState>();
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;   /* no imgui.ini next to the executable */
		io.LogFilename = nullptr;
		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding   = 0.0f;
		style.WindowBorderSize = 0.0f;
		style.FrameRounding    = 3.0f;

		if (!ImGui_ImplGlfw_InitForVulkan(glfwWindow, true)) {
			ImGui::DestroyContext();
			[[unlikely]] throw std::runtime_error("Failed to initialise ImGui GLFW backend");
		}

		/* Descriptor pool for the backend's font atlas and any user textures.
		 * ImGui 1.93 uses separate SAMPLED_IMAGE and SAMPLER descriptors. */
		VkDescriptorPoolSize sizes[3] = {
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          64 },
			{ VK_DESCRIPTOR_TYPE_SAMPLER,                64 },
		};
		VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pci.maxSets       = 64;
		pci.poolSizeCount = 3;
		pci.pPoolSizes    = sizes;
		if (vkCreateDescriptorPool(vk_.Device(), &pci, nullptr, &imguiPool_) != VK_SUCCESS) {
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			[[unlikely]] throw std::runtime_error("Failed to create the ImGui descriptor pool");
		}

		ImGui_ImplVulkan_InitInfo info{};
		info.ApiVersion        = VK_API_VERSION_1_1;
		info.Instance          = vk_.Instance();
		info.PhysicalDevice    = vk_.PhysicalDevice();
		info.Device            = vk_.Device();
		info.QueueFamily       = vk_.GraphicsFamily();
		info.Queue             = vk_.GraphicsQueue();
		info.DescriptorPool    = imguiPool_;
		info.MinImageCount     = vk_.ImageCount();
		info.ImageCount        = vk_.ImageCount();
		info.UseDynamicRendering = false;
		/* 1.93: the render pass lives in PipelineInfoMain, not in InitInfo. */
		info.PipelineInfoMain.RenderPass   = renderer_.RenderPass();
		info.PipelineInfoMain.Subpass      = 0;
		info.PipelineInfoMain.MSAASamples  = VK_SAMPLE_COUNT_1_BIT;

		/* Fonts: 1.93 uploads the atlas on the first NewFrame(); there is no
		 * ImGui_ImplVulkan_CreateFontsTexture() in this version any more. */
		if (!ImGui_ImplVulkan_Init(&info)) {
			vkDestroyDescriptorPool(vk_.Device(), imguiPool_, nullptr);
			imguiPool_ = VK_NULL_HANDLE;
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			[[unlikely]] throw std::runtime_error("Failed to initialise the ImGui Vulkan backend");
		}
		imgui_->initialized = true;
	}

	void ShutdownImGui()
	{
		if (!imgui_ || !imgui_->initialized) return;
		vk_.WaitIdle();
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		imgui_->initialized = false;
		if (imguiPool_ != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(vk_.Device(), imguiPool_, nullptr);
			imguiPool_ = VK_NULL_HANDLE;
		}
	}

	/* ── Image → GPU ───────────────────────────────────────────────────────── */

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

	/* Exactly the plugin's target format: interleaved 8-bit RGBA, in order. */
	static bool IsPackedRgba8(const IWImageFormat& f) noexcept
	{
		if (f.storageLayout != IW_STORAGE_INTERLEAVED) return false;
		if (f.componentCount != 4u || f.bitsPerPixel != 32u) return false;
		static const IWComponentSemantic kOrder[4] = {
			IW_COMPONENT_SEMANTIC_R, IW_COMPONENT_SEMANTIC_G,
			IW_COMPONENT_SEMANTIC_B, IW_COMPONENT_SEMANTIC_A
		};
		for (int i = 0; i < 4; ++i) {
			const IWComponentDef& c = f.components[i];
			if (c.semantic       != kOrder[i])               return false;
			if (c.componentClass != IW_COMPONENT_CLASS_UNORM) return false;
			if (c.bitWidth       != 8u)                      return false;
			if (c.bitOffset      != (uint16_t)(i * 8))       return false;
		}
		return true;
	}

	/* Convert one frame to tightly packed RGBA8.  The plugin already produces
	 * exactly that layout, so the common path is a straight row copy; anything
	 * else (float/16-bit/planar/exotic semantics when no plugin is loaded) is
	 * normalised through the same FilterCore helper the plugin uses. */
	static void ToRgba8(const ImagePluginData& pd, std::vector<uint8_t>& out)
	{
		out.resize((size_t)pd.width * (size_t)pd.height * 4u);
		if (IsPackedRgba8(pd.format)) {
			for (int y = 0; y < pd.height; ++y)
				std::memcpy(out.data() + (size_t)y * pd.width * 4u,
				            pd.data  + (ptrdiff_t)y * pd.stride,
				            (size_t)pd.width * 4u);
			return;
		}

		for (int y = 0; y < pd.height; ++y) {
			uint8_t* dst = out.data() + (size_t)y * pd.width * 4u;
			for (int x = 0; x < pd.width; ++x) {
				float rgba[4];
				FilterCore::ExtractPixelF(pd, x, y, rgba);
				for (int c = 0; c < 4; ++c) {
					const float v = std::clamp(rgba[c], 0.0f, 1.0f);
					dst[x * 4 + c] = (uint8_t)std::lround(v * 255.0f);
				}
			}
		}
	}

public:
	/* Kept under its OpenGL name so the call sites do not change.  The tiling
	 * parameters are meaningless for a single Vulkan texture and are ignored.
	 *
	 * Runs the filter plugin (which does the actual temporal quantisation) and
	 * uploads at most two phase frames.  Returns false when the plugin produced
	 * more than two frames — N-phase is out of scope for this build. */
	bool LoadTextures(
		const Image& image,
		const int /*segmentWidth*/,
		const int /*segmentHeight*/,
		const int /*redundantBorderSize*/,
		const IWFilterOptions& opts = IWFilterOptions{
			IW_FILTER_OPTIONS_VERSION, 0u, nullptr })
	{
		if (image.width <= 0 || image.height <= 0)
			return false;

		std::vector<uint8_t> phaseA;
		std::vector<uint8_t> phaseB;

		if (filterPlugin_.IsLoaded()) {
			const ImagePluginData pd = MakePluginDataView(image);
			/* 8 bits per channel, 4 channels: exactly what the swapchain and
			 * both FBOs offer, and what the OpenGL renderer reported. */
			const IWScreenInfo screen{ 8u, 4u };

			IWFilterImageSet* set = nullptr;
			try { set = filterPlugin_.Filter(&pd, &screen, &opts); }
			catch (...) { set = nullptr; }

			if (set && set->frameCount > 0 && set->frames) {
				if (set->frameCount > 2u) {
					std::fprintf(stderr,
						"iview: pwm_bits > 1 (%u phase frames) is not implemented in this build\n",
						set->frameCount);
					filterPlugin_.Free(set);
					return false;
				}
				for (uint32_t i = 0; i < set->frameCount; ++i) {
					if (!set->frames[i]) continue;
					try {
						if (i == 0) ToRgba8(*set->frames[i], phaseA);
						else        ToRgba8(*set->frames[i], phaseB);
					} catch (...) { /* skip a frame that fails to convert */ }
				}
				filterPlugin_.Free(set);
			} else if (set) {
				filterPlugin_.Free(set);
			}
		}

		if (phaseA.empty()) {
			/* No filter (or it returned nothing): use the image directly. */
			ToRgba8(MakePluginDataView(image), phaseA);
		}

		renderer_.Upload((uint32_t)image.width, (uint32_t)image.height,
		                 phaseA.data(), phaseB.empty() ? nullptr : phaseB.data());

		imageWidth_  = image.width;
		imageHeight_ = image.height;
		CenterImage();
		return true;
	}

	/* Filter options from the command line.  The argument array is copied into
	 * the Window, so the caller's storage does not have to outlive Main(). */
	void SetFilterOptions(const IWFilterOptions& opts)
	{
		const uint32_t count = std::min<uint32_t>(opts.argc, 5u);
		for (uint32_t i = 0; i < count; ++i) filterArgStorage_[i] = opts.argv[i];
		filterOptions_.version = opts.version;
		filterOptions_.argc    = count;
		filterOptions_.argv    = (count != 0u) ? filterArgStorage_ : nullptr;
	}

	void CenterImage()
	{
		setPanning(0.0, 0.0);
		if (HasImage()) {
			double minRatio = std::min<double>((double)width / imageWidth_, (double)height / imageHeight_);
			this->scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
	}

	/* ── Main loop ─────────────────────────────────────────────────────────── */

	void Main(std::future<Image>&& imageFuture, std::filesystem::path imagePath)
	{
		using Clock = std::chrono::steady_clock;

		bool uploaded = false;
		bool shown    = false;
		bool imguiReady = false;
		bool dirty    = false;
		bool firstIteration = true;
		uint32_t phase = 0;       /* which FBO is scheduled for the next copy */
		uint64_t slot  = 0;       /* frame slot, == swapchain image count    */

		/* Diagnostics (only printed when IMAGEVIEWER_VK_TRACE is set). */
		const bool trace = IviewTraceEnabled();
		std::vector<double> presentIntervalsMs;
		double cpuOutsidePresentMs = 0.0;
		bool   idleRecipePrinted   = false;
		auto   lastPresentEnd      = Clock::now();

		while (!glfwWindowShouldClose(glfwWindow))
		{
			const auto iterationStart = Clock::now();
			glfwPollEvents();

			/* (1) Decode future.  Before the image exists there is nothing to
			 * present, so no acquire and no present happen: poll the future and
			 * wait for events for at most 2 ms. */
			if (!uploaded)
			{
				if (firstIteration) { IviewTrace("first loop iteration (hidden window)"); firstIteration = false; }

				if (imageFuture.valid() &&
				    imageFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				{
					IviewTrace("future ready");
					try
					{
						const Image image = imageFuture.get();
						if (!LoadTextures(image, 0, 0, 0, filterOptions_))
							throw std::runtime_error("the filter produced an unsupported frame count");
						SetWindowTitle(imagePath);
						ComputeReadout(image, imagePath);
						uploaded = true;
						dirty = true;
						IviewTrace("image uploaded to the GPU");
					}
					catch (std::exception& ex)
					{
#ifdef _WIN32
						MessageBoxA(NULL, ex.what(), "Error", MB_OK | MB_ICONERROR);
#else
						std::cerr << "Failed to process the request : " << ex.what() << std::endl;
#endif
						break;
					}
				}
				else
				{
					/* The only wait in the loop, and only while nothing exists to
					 * present; afterwards FIFO presentation paces everything. */
					glfwWaitEventsTimeout(0.002);
					continue;
				}
			}

			if (firstIteration) { IviewTrace("first loop iteration"); firstIteration = false; }

			/* (2) Swapchain recreation (resize / OUT_OF_DATE / SUBOPTIMAL). */
			if (resizePending_)
			{
				HandleResize(dirty);
			}

			/* (3) One ImGui frame per dirty burst, reused by both FBO passes. */
			ImDrawData* guiData = nullptr;
			if (dirty && imguiReady)
			{
				ImGui_ImplVulkan_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();
				BuildChrome();
				ImGui::Render();
				guiData = ImGui::GetDrawData();
			}

			/* (4) Acquire, then either the dirty command buffer (two FBO draws +
			 * one copy) or the pre-recorded idle copy. */
			uint32_t imageIndex = 0;
			if (!vk_.AcquireNextImage((uint32_t)(slot % vk_.ImageCount()), &imageIndex))
			{
				resizePending_ = true;
				continue;
			}

			const bool dirtyFrame = dirty;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			if (dirtyFrame)
			{
				cmd = renderer_.DirtyCommand((uint32_t)(slot % vk_.ImageCount()));
				renderer_.BeginCommand(cmd);

				const ImageDrawParams p = ComputeDrawParams();
				for (uint32_t ph = 0; ph < 2u; ++ph)
				{
					renderer_.BeginImagePass(cmd, ph, p);
					if (guiData != nullptr)
						ImGui_ImplVulkan_RenderDrawData(guiData, cmd);
					renderer_.EndImagePass(cmd);
				}
				/* The dirty pass always presents phase A; the alternation
				 * resumes from phase B on the next frame. */
				renderer_.CopyPhaseToSwapchain(cmd, imageIndex, 0);
				renderer_.EndCommand(cmd);

				if (!shown) IviewTrace("dirty pass recorded (both FBOs)");
			}
			else
			{
				cmd = renderer_.IdleCommand(imageIndex, phase);
			}

			const auto beforePresent = Clock::now();
			const bool presented = vk_.SubmitAndPresent((uint32_t)(slot % vk_.ImageCount()),
			                                            imageIndex, cmd);
			const auto afterPresent = Clock::now();

			if (!presented)
			{
				resizePending_ = true;
				continue;
			}

			if (dirtyFrame)
			{
				dirty = false;
				phase = 1u;
				if (!shown)
				{
					IviewTrace("first frame presented (image, still hidden)");
					/* The image is already in the swapchain image that was just
					 * presented; only now does the window become visible, so the
					 * first frame the user ever sees is the decoded image. */
					glfwShowWindow(glfwWindow);
					shown = true;
					imguiReady = true;
					IviewTrace("glfwShowWindow");
					/* Chrome must be drawn on the next frame. */
					dirty = true;
				}
			}
			else
			{
				phase ^= 1u;
				if (!idleRecipePrinted && trace)
				{
					idleRecipePrinted = true;
					std::fprintf(stderr,
						"IVIEW_TRACE idle frame: vkAcquireNextImageKHR + "
						"vkCmdPipelineBarrier(UNDEFINED->TRANSFER_DST) + "
						"vkCmdCopyImage(FBO[%u] -> image %u) + "
						"vkCmdPipelineBarrier(TRANSFER_DST->PRESENT_SRC) + "
						"vkQueueSubmit + vkQueuePresentKHR  (no render pass, no draw)\n",
						phase ^ 1u, imageIndex);
					std::fflush(stderr);
				}
			}

			slot++;

			/* (5) Diagnostics: FIFO pacing and CPU cost outside the present.
			 * Only idle frames are sampled — a dirty burst is not representative. */
			if (trace)
			{
				if (!dirtyFrame && presentIntervalsMs.size() < 120)
				{
					cpuOutsidePresentMs +=
						std::chrono::duration<double, std::milli>(beforePresent - iterationStart).count();
					presentIntervalsMs.push_back(
						std::chrono::duration<double, std::milli>(afterPresent - lastPresentEnd).count());
					if (presentIntervalsMs.size() == 120)
					{
						std::vector<double> sorted = presentIntervalsMs;
						std::sort(sorted.begin(), sorted.end());
						std::fprintf(stderr,
							"IVIEW_TRACE 120 idle presents: median interval %.2f ms "
							"(min %.2f, max %.2f); CPU outside present %.3f ms/frame\n",
							sorted[sorted.size() / 2], sorted.front(), sorted.back(),
							cpuOutsidePresentMs / 120.0);
						std::fflush(stderr);
					}
				}
				lastPresentEnd = afterPresent;
			}

			if (dirtyRequest_)
			{
				dirtyRequest_ = false;
				dirty = true;
			}

			/* The fade needs frames while it animates, and the 1.5 s idle
			 * transition needs one frame at the moment it starts. */
			if (imguiReady && ChromeNeedsFrame()) dirty = true;
		}

		ShutdownImGui();

		/* Step-7 FBO preservation check (opt-in: it copies 60 frames). */
		if (const char* selftest = std::getenv("IMAGEVIEWER_VK_SELFTEST");
		    selftest != nullptr && selftest[0] != '\0' && std::strcmp(selftest, "0") != 0)
		{
			renderer_.SelfTestFboPreservation();
		}

		renderer_.Shutdown();
		vk_.Shutdown();
	}

private:
	/* NDC placement of the image quad, derived exactly like the OpenGL
	 * renderer's orthographic projection. */
	ImageDrawParams ComputeDrawParams() const
	{
		ImageDrawParams p;
		if (!HasImage() || width <= 0 || height <= 0) return p;

		const double zoom = getZoom();
		const double imgW = imageWidth_ * zoom;
		const double imgH = imageHeight_ * zoom;
		const double imgX = (width - imgW) * 0.5 + panX;
		const double imgY = (height - imgH) * 0.5 + panY;

		p.posX  = (float)(2.0 * imgX / width  - 1.0);
		p.posY  = (float)(2.0 * imgY / height - 1.0);
		p.sizeX = (float)(2.0 * imgW / width);
		p.sizeY = (float)(2.0 * imgH / height);
		p.nearest = (zoom >= 1.0);
		return p;
	}

	void HandleResize(bool& dirty)
	{
		resizePending_ = false;

		int fbWidth = 0, fbHeight = 0;
		glfwGetFramebufferSize(glfwWindow, &fbWidth, &fbHeight);
		if (fbWidth <= 0 || fbHeight <= 0) return;   /* minimised: keep the old swapchain */

		width  = fbWidth;
		height = fbHeight;

		vk_.WaitIdle();
		if (!vk_.RecreateSwapchain((uint32_t)fbWidth, (uint32_t)fbHeight))
		{
			resizePending_ = true;   /* try again next iteration */
			return;
		}
		/* Same call path as the initial creation: new FBOs, new idle buffers. */
		renderer_.OnSwapchainChanged();
		ImGui_ImplVulkan_SetMinImageCount(vk_.MinImageCount());
		dirty = true;
		IviewTrace("swapchain recreated");
	}

	/* True while the chrome alpha still has to move towards its target. */
	bool ChromeNeedsFrame() const
	{
		if (!imgui_) return false;
		using Clock = std::chrono::steady_clock;
		const auto now = Clock::now();
		const bool idle = (now - imgui_->lastMouseActivity) > std::chrono::milliseconds(1500);
		const float target = !imgui_->chromeEnabled ? 0.0f : (idle ? 0.15f : 1.0f);
		return std::fabs(imgui_->alpha - target) > 0.0001f;
	}

	void SetWindowTitle(const std::filesystem::path& path)
	{
		const std::string title = "ImageViewer \xE2\x80\x94 " + Utf8Filename(path);
		glfwSetWindowTitle(glfwWindow, title.c_str());
	}

	void ComputeReadout(const Image& image, const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
		if (ec)
		{
			imgui_->readout = std::format("{}\xC3\x97{}", image.width, image.height);
		}
		else
		{
			const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
			imgui_->readout = std::format("{}\xC3\x97{} \xC2\xB7 {:.1f} MB",
			                              image.width, image.height, megabytes);
		}
	}

	static std::string Utf8Filename(const std::filesystem::path& path)
	{
		const std::u8string utf8 = path.filename().u8string();
		return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}

	/* Fade + chrome.  Alpha slews 0 -> 1 in 200 ms, idles at 0.15 after 1.5 s
	 * without mouse motion, and drops to 0 when Tab hides the chrome. */
	void BuildChrome()
	{
		using Clock = std::chrono::steady_clock;
		const auto now = Clock::now();

		if (!imgui_->frameClockStarted)
		{
			imgui_->lastMouseActivity = now;
			imgui_->frameClockStarted = true;
		}

		const ImGuiIO& io = ImGui::GetIO();
		if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
			imgui_->lastMouseActivity = now;

		const bool idle = (now - imgui_->lastMouseActivity) > std::chrono::milliseconds(1500);
		float target = 0.15f;
		if (!imgui_->chromeEnabled)      target = 0.0f;
		else if (!idle)                  target = 1.0f;

		const float step = (io.DeltaTime > 0.0f) ? (io.DeltaTime / 0.2f) : 1.0f;
		if (imgui_->alpha < target) imgui_->alpha = std::min(target, imgui_->alpha + step);
		else                        imgui_->alpha = std::max(target, imgui_->alpha - step);

		ImGui::GetStyle().Alpha = imgui_->alpha;
		if (imgui_->alpha <= 0.001f) return;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		constexpr ImGuiWindowFlags kChromeFlags =
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize;

		/* Top-right readout: dimensions and on-disk size, computed once. */
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + 10.0f),
		                        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.85f));
		if (ImGui::Begin("##readout", nullptr, kChromeFlags | ImGuiWindowFlags_NoInputs))
		{
			ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
			ImGui::TextUnformatted(imgui_->readout.c_str());
			ImGui::PopFont();
		}
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		/* Bottom-centre control strip: no decoration, transparent background. */
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
		                               vp->WorkPos.y + vp->WorkSize.y - 12.0f),
		                        ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
		if (ImGui::Begin("##controls", nullptr, kChromeFlags))
		{
			if (ImGui::Button("Fit")) fitToWindow();
			ImGui::SameLine();
			if (ImGui::Button("1:1")) setZoom(1.0);

			ImGui::SameLine(0.0f, 14.0f);
			if (ImGui::Button("-")) adjustZoom(-10);
			ImGui::SameLine();
			char zoomLabel[16];
			std::snprintf(zoomLabel, sizeof(zoomLabel), "%3d%%", getZoomPercent());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(zoomLabel);
			ImGui::SameLine();
			if (ImGui::Button("+")) adjustZoom(10);

			ImGui::SameLine(0.0f, 14.0f);
			if (ImGui::Button("Filter")) ImGui::OpenPopup("##filters");
			if (ImGui::BeginPopup("##filters"))
			{
				ImGui::TextDisabled("No filters yet");
				ImGui::EndPopup();
			}
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}

	/* File drop: phase 1 reuses the same decode + upload path on the main
	 * thread (this runs inside glfwPollEvents, between two frames). */
	void HandleDrop(int count, const char** paths)
	{
		if (count <= 0 || paths == nullptr || !imgui_) return;

		for (int i = 0; i < count; ++i)
		{
			if (paths[i] == nullptr) continue;
			try
			{
				const std::filesystem::path path(reinterpret_cast<const char8_t*>(paths[i]));
				const Image image = Image::FromFile(path);
				if (!LoadTextures(image, 0, 0, 0, filterOptions_))
					continue;
				SetWindowTitle(path);
				ComputeReadout(image, path);
				imgui_->chromeEnabled     = true;
				imgui_->lastMouseActivity = std::chrono::steady_clock::now();
				return;
			}
			catch (const std::exception&)
			{
				/* Try the next dropped file; nothing to report in phase 1. */
			}
		}
	}

public:
	void setZoom(double zoomLevel) {
		setZoom(zoomLevel, width * 0.5, height * 0.5);
	}

	void setZoom(double zoomLevel, double cursorX, double cursorY) {
        const double currentZoom = getZoom();
        const double newScroll = (zoomLevel * 100.0) - 100.0;
        scroll = std::clamp(static_cast<int>(newScroll), -90, 400);

        if (HasImage()) {
            const double imgW = imageWidth_ * currentZoom;
            const double imgH = imageHeight_ * currentZoom;
            const double imgX = (width - imgW) * 0.5 + panX;
            const double imgY = (height - imgH) * 0.5 + panY;

            const double mouseImgXBefore = (cursorX - imgX) / currentZoom;
            const double mouseImgYBefore = (cursorY - imgY) / currentZoom;

            const double newZoom = getZoom();
            const double newImgW = imageWidth_ * newZoom;
            const double newImgH = imageHeight_ * newZoom;
            const double newImgX = (width - newImgW) * 0.5 + panX;
            const double newImgY = (height - newImgH) * 0.5 + panY;
            const double mouseScreenXAfter = newImgX + mouseImgXBefore * newZoom;
            const double mouseScreenYAfter = newImgY + mouseImgYBefore * newZoom;

            setPanning(panX + cursorX - mouseScreenXAfter, panY + cursorY - mouseScreenYAfter);
        }
        dirtyRequest_ = true;
    }

	void adjustZoom(int zoomDelta) {
        const double currentZoom = getZoom();
        scroll = std::clamp(scroll + zoomDelta, -90, 400);

        if (HasImage()) {
            const double imgW = imageWidth_ * currentZoom;
            const double imgH = imageHeight_ * currentZoom;
            const double imgX = (width - imgW) * 0.5 + panX;
            const double imgY = (height - imgH) * 0.5 + panY;

            const double centerX = width * 0.5;
            const double centerY = height * 0.5;
            const double mouseImgXBefore = (centerX - imgX) / currentZoom;
            const double mouseImgYBefore = (centerY - imgY) / currentZoom;

            const double newZoom = getZoom();
            const double newImgW = imageWidth_ * newZoom;
            const double newImgH = imageHeight_ * newZoom;
            const double newImgX = (width - newImgW) * 0.5 + panX;
            const double newImgY = (height - newImgH) * 0.5 + panY;
            const double mouseScreenXAfter = newImgX + mouseImgXBefore * newZoom;
            const double mouseScreenYAfter = newImgY + mouseImgYBefore * newZoom;

            setPanning(panX + centerX - mouseScreenXAfter, panY + centerY - mouseScreenYAfter);
        }
        dirtyRequest_ = true;
    }

	void adjustPan(int dx, int dy) {
		setPanning(panX + dx, panY + dy);
	}

	void fitToWindow() {
		setPanning(0.0, 0.0);
		if (HasImage()) {
			const double minRatio = std::min(static_cast<double>(width) / imageWidth_,
											 static_cast<double>(height) / imageHeight_);
			scroll = static_cast<int>(minRatio * 100.0 - 100);
		}
		dirtyRequest_ = true;
	}

	static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
		Window* containerWindow = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (containerWindow) {
			/* Tab toggles the chrome.  That this callback still runs is also the
			 * proof that ImGui_ImplGlfw_InitForVulkan(..., true) chained the
			 * previously installed GLFW callbacks rather than replacing them. */
			if (key == GLFW_KEY_TAB && action == GLFW_PRESS && containerWindow->imgui_) {
				containerWindow->imgui_->chromeEnabled = !containerWindow->imgui_->chromeEnabled;
				containerWindow->imgui_->lastMouseActivity = std::chrono::steady_clock::now();
			}
			containerWindow->hotkeysHandler.key_callback(window, key, scancode, action, mods);
			containerWindow->dirtyRequest_ = true;
		}
	}
};
