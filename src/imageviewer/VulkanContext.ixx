module;

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

/* Volk is the single Vulkan entry point: it declares every vk* symbol as a
 * function pointer (because VK_NO_PROTOTYPES is defined) and loads them from
 * the platform loader in volkInitialize().  Nothing links against vulkan-1.lib
 * directly. */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

export module VulkanContext;

/* ── Trace helper ─────────────────────────────────────────────────────────────
 *
 * Enabled only when IMAGEVIEWER_VK_TRACE is set to something other than "0".
 * All timestamps are milliseconds since the first trace call in the process
 * (the static below runs during static initialisation, i.e. before main()).
 * It lives here because both the window module and the context want it and a
 * single definition keeps the clock shared. */
static const std::chrono::steady_clock::time_point& IviewTraceStart() noexcept
{
	static const std::chrono::steady_clock::time_point t0 =
		std::chrono::steady_clock::now();
	return t0;
}

export bool IviewTraceEnabled() noexcept
{
	static const bool enabled = [] {
		const char* v = std::getenv("IMAGEVIEWER_VK_TRACE");
		return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
	}();
	return enabled;
}

export void IviewTrace(const char* what) noexcept
{
	if (!IviewTraceEnabled()) return;
	const auto t0 = IviewTraceStart();
	const double ms = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	std::fprintf(stderr, "IVIEW_TRACE +%8.2f ms  %s\n", ms, what);
	std::fflush(stderr);
}

export class VulkanContext
{
public:
	VulkanContext() = default;
	~VulkanContext() { Shutdown(); }
	VulkanContext(const VulkanContext&)            = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;

	/* Creates instance, surface, device, queues, command pool and the first
	 * swapchain.  Throws std::runtime_error on failure. */
	void Initialize(GLFWwindow* window, uint32_t width, uint32_t height)
	{
		if (volkInitialize() != VK_SUCCESS)
			throw std::runtime_error("Vulkan: volkInitialize failed (no Vulkan loader found)");
		IviewTrace("volk initialized");

		CreateInstance();
		IviewTrace("vkCreateInstance done");

		CreateSurface(window);
		IviewTrace("glfwCreateWindowSurface done");

		PickPhysicalDevice();
		CreateDevice();
		IviewTrace("vkCreateDevice done");

		CreateCommandPool();

		if (!CreateSwapchain(width, height, VK_NULL_HANDLE))
			throw std::runtime_error("Vulkan: failed to create the initial swapchain");
		IviewTrace("vkCreateSwapchainKHR done");
	}

	void Shutdown()
	{
		const bool wasInitialized = (instance_ != VK_NULL_HANDLE);
		if (device_ != VK_NULL_HANDLE)
			vkDeviceWaitIdle(device_);
		DestroySwapchain();
		if (device_ != VK_NULL_HANDLE)
		{
			if (commandPool_ != VK_NULL_HANDLE)
				vkDestroyCommandPool(device_, commandPool_, nullptr);
			vkDestroyDevice(device_, nullptr);
			device_ = VK_NULL_HANDLE;
		}
		if (instance_ != VK_NULL_HANDLE)
		{
			if (surface_ != VK_NULL_HANDLE)
			{
				vkDestroySurfaceKHR(instance_, surface_, nullptr);
				surface_ = VK_NULL_HANDLE;
			}
			if (debugMessenger_ != VK_NULL_HANDLE)
			{
				vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
				debugMessenger_ = VK_NULL_HANDLE;
			}
			vkDestroyInstance(instance_, nullptr);
			instance_ = VK_NULL_HANDLE;
		}
		if (wasInitialized)
			volkFinalize();   /* unload the loader and NULL every entry point */
	}

	/* Recreates the swapchain (and its per-image layout bookkeeping) at a new
	 * extent.  The caller must have called vkDeviceWaitIdle or otherwise know
	 * that no submitted work references the old swapchain images; Window does
	 * the wait before calling this. */
	bool RecreateSwapchain(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0) return false;
		vkDeviceWaitIdle(device_);
		return CreateSwapchain(width, height, swapchain_);
	}

	/* ── Accessors ─────────────────────────────────────────────────────────── */

	VkInstance       Instance()        const noexcept { return instance_; }
	VkPhysicalDevice PhysicalDevice()  const noexcept { return physicalDevice_; }
	VkDevice         Device()          const noexcept { return device_; }
	VkSurfaceKHR     Surface()         const noexcept { return surface_; }
	VkCommandPool    CommandPool()     const noexcept { return commandPool_; }
	VkQueue          GraphicsQueue()   const noexcept { return graphicsQueue_; }
	VkQueue          PresentQueue()    const noexcept { return presentQueue_; }
	uint32_t         GraphicsFamily()  const noexcept { return graphicsFamily_; }
	uint32_t         PresentFamily()   const noexcept { return presentFamily_; }
	const char*      DeviceName()      const noexcept { return deviceName_; }

	VkSwapchainKHR Swapchain()         const noexcept { return swapchain_; }
	VkFormat       SwapchainFormat()   const noexcept { return swapchainFormat_; }
	VkExtent2D     Extent()            const noexcept { return extent_; }
	uint32_t       ImageCount()        const noexcept { return (uint32_t)swapImages_.size(); }
	VkImage        Image(uint32_t i)   const noexcept { return swapImages_[i]; }
	uint32_t       MinImageCount()     const noexcept { return minImageCount_; }
	VkPresentModeKHR PresentMode()     const noexcept { return presentMode_; }
	const char*    PresentModeName()   const noexcept
	{
		switch (presentMode_)
		{
		case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
		case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
		case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
		default:                               return "OTHER";
		}
	}

	/* ── Frame submission ──────────────────────────────────────────────────── */

	/* Acquires the next image for frame slot `slot`.  Returns false when the
	 * swapchain is out of date / the surface was lost; the caller must recreate
	 * and retry.  On success the slot's fence has been waited on and reset, so
	 * the slot's command buffer may be re-recorded. */
	bool AcquireNextImage(uint32_t slot, uint32_t* imageIndexOut)
	{
		const VkFence fence = fences_[slot];
		vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);

		VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
		                                   acquireSemaphores_[slot], VK_NULL_HANDLE,
		                                   imageIndexOut);
		if (traceFrames_ < 4) IviewTrace("acquire done");
		if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
		if (r == VK_ERROR_SURFACE_LOST_KHR) return false;
		if (r == VK_SUBOPTIMAL_KHR)         return true;  /* usable for now */
		if (r != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAcquireNextImageKHR failed");
		return true;
	}

	/* Submits one command buffer that writes swapchain image `imageIndex` and
	 * presents it.  FIFO presentation is the loop's only pacing device, so this
	 * call blocks at the vsync boundary. */
	bool SubmitAndPresent(uint32_t slot, uint32_t imageIndex, VkCommandBuffer cmd)
	{
		const VkSemaphore wait = acquireSemaphores_[slot];
		const VkSemaphore signal = renderFinishedSemaphores_[slot];

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount   = 1;
		submit.pWaitSemaphores      = &wait;
		submit.pWaitDstStageMask    = &waitStage;
		submit.commandBufferCount   = 1;
		submit.pCommandBuffers      = &cmd;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &signal;

		vkResetFences(device_, 1, &fences_[slot]);
		VkResult r = vkQueueSubmit(graphicsQueue_, 1, &submit, fences_[slot]);
		if (r != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkQueueSubmit failed");
		if (traceFrames_ < 4) IviewTrace("submit returned");

		VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores    = &signal;
		present.swapchainCount     = 1;
		present.pSwapchains        = &swapchain_;
		present.pImageIndices      = &imageIndex;

		const auto presentStart = std::chrono::steady_clock::now();
		r = vkQueuePresentKHR(presentQueue_, &present);
		if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
			return false;
		if (r == VK_SUBOPTIMAL_KHR)
			return false;  /* still presented; caller recreates before the next frame */
		if (r != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkQueuePresentKHR failed");
				const auto presentEnd = std::chrono::steady_clock::now();
		++traceFrames_;
		++presentCount_;
		if (presentCount_ <= 5 && IviewTraceEnabled())
		{
			if (presentCount_ == 1) lastPresentEnd_ = presentStart;
			const double callMs =
				std::chrono::duration<double, std::milli>(presentEnd - presentStart).count();
			const double intervalMs =
				std::chrono::duration<double, std::milli>(presentEnd - lastPresentEnd_).count();
			std::fprintf(stderr,
				"IVIEW_TRACE present #%u: call %.2f ms, interval %.2f ms\n",
				presentCount_, callMs, intervalMs);
			std::fflush(stderr);
			lastPresentEnd_ = presentEnd;
		}
		return true;
	}

		void WaitIdle() const
	{
		if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
	}

	/* Waits on both queues (graphics and present may share a queue).  The
	 * warm-up uses this so the compositor's first-flip cost is paid before the
	 * window is shown instead of on the first image frame. */
	void QueueWaitIdle() const
	{
		if (graphicsQueue_ != VK_NULL_HANDLE) vkQueueWaitIdle(graphicsQueue_);
		if (presentQueue_ != VK_NULL_HANDLE && presentQueue_ != graphicsQueue_)
			vkQueueWaitIdle(presentQueue_);
	}

private:
	/* ── Instance ──────────────────────────────────────────────────────────── */

	static bool ValidationRequested() noexcept
	{
#ifdef _DEBUG
		return true;
#else
		const char* v = std::getenv("IMAGEVIEWER_VK_VALIDATION");
		return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
#endif
	}

	static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT /*types*/,
		const VkDebugUtilsMessengerCallbackDataEXT* data,
		void* /*user*/)
	{
		const char* level = "info";
		if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        level = "error";
		else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = "warning";
		std::fprintf(stderr, "[vk %s] %s\n", level, data->pMessage);
		std::fflush(stderr);
		return VK_FALSE;
	}

	void CreateInstance()
	{
		uint32_t count = 0;
		const char** required = glfwGetRequiredInstanceExtensions(&count);
		if (required == nullptr || count == 0)
			throw std::runtime_error("Vulkan: GLFW could not determine the required instance extensions");

		std::vector<const char*> extensions(required, required + count);

		const bool wantValidation = ValidationRequested();
		bool haveValidation = false;
		if (wantValidation)
		{
			uint32_t layerCount = 0;
			vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> layers(layerCount);
			vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
			for (const auto& l : layers)
				if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
					haveValidation = true;

			if (haveValidation)
				extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			else
				std::fprintf(stderr, "Warning: Vulkan validation requested but not installed\n");
		}

		uint32_t apiVersion = VK_API_VERSION_1_0;
		if (vkEnumerateInstanceVersion != nullptr)
		{
			uint32_t v = 0;
			if (vkEnumerateInstanceVersion(&v) == VK_SUCCESS && v >= VK_API_VERSION_1_1)
				apiVersion = VK_API_VERSION_1_1;
		}

		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "ImageViewer";
		app.applicationVersion = 1;
		app.pEngineName = "ImageViewer";
		app.engineVersion = 1;
		app.apiVersion = apiVersion;

		const char* validationLayer = "VK_LAYER_KHRONOS_validation";

		VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ci.pApplicationInfo        = &app;
		ci.enabledExtensionCount   = (uint32_t)extensions.size();
		ci.ppEnabledExtensionNames = extensions.data();
		if (haveValidation)
		{
			ci.enabledLayerCount   = 1;
			ci.ppEnabledLayerNames = &validationLayer;
		}

		VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
		if (r != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateInstance failed");

		volkLoadInstance(instance_);

		if (haveValidation)
		{
			VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
			                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
			dbg.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			dbg.pfnUserCallback = DebugCallback;
			vkCreateDebugUtilsMessengerEXT(instance_, &dbg, nullptr, &debugMessenger_);
		}
	}

	void CreateSurface(GLFWwindow* window)
	{
		VkResult r = glfwCreateWindowSurface(instance_, window, nullptr, &surface_);
		if (r != VK_SUCCESS || surface_ == VK_NULL_HANDLE)
			throw std::runtime_error(
				"Vulkan: glfwCreateWindowSurface failed — the window has no Vulkan-capable surface "
				"(check that the display server / driver supports Vulkan presentation)");
	}

	/* ── Device ────────────────────────────────────────────────────────────── */

	static bool HasSwapchainExtension(VkPhysicalDevice dev)
	{
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> props(count);
		vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data());
		for (const auto& p : props)
			if (std::strcmp(p.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
				return true;
		return false;
	}

	void PickPhysicalDevice()
	{
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance_, &count, nullptr);
		if (count == 0)
			throw std::runtime_error("Vulkan: no physical devices found");
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance_, &count, devices.data());

		int bestScore = -1;
		for (VkPhysicalDevice dev : devices)
		{
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(dev, &props);
			if (props.apiVersion < VK_API_VERSION_1_1) continue;
			if (!HasSwapchainExtension(dev)) continue;

			/* Graphics family. */
			uint32_t familyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, families.data());

			int32_t graphics = -1, present = -1;
			for (uint32_t i = 0; i < familyCount; ++i)
			{
				if (families[i].queueCount == 0) continue;
				if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
					if (graphics < 0) graphics = (int32_t)i;
				VkBool32 supportsPresent = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &supportsPresent);
				if (supportsPresent && present < 0) present = (int32_t)i;
			}
			if (graphics < 0 || present < 0) continue;

			uint32_t formatCount = 0, modeCount = 0;
			vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &formatCount, nullptr);
			vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &modeCount, nullptr);
			if (formatCount == 0 || modeCount == 0) continue;

			int score = 100;
			switch (props.deviceType)
			{
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 1000; break;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500;  break;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 300;  break;
			case VK_PHYSICAL_DEVICE_TYPE_CPU:            score = 0;    break;
			default: break;
			}
			if (score > bestScore)
			{
				bestScore = score;
				physicalDevice_ = dev;
				graphicsFamily_ = (uint32_t)graphics;
				presentFamily_  = (uint32_t)present;
				std::snprintf(deviceName_, sizeof(deviceName_), "%s", props.deviceName);
			}
		}

		if (physicalDevice_ == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan: no physical device supports graphics + presentation + swapchain");
	}

	void CreateDevice()
	{
		const float priority = 1.0f;

		std::vector<VkDeviceQueueCreateInfo> queueInfos;
		VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		q.queueFamilyIndex = graphicsFamily_;
		q.queueCount       = 1;
		q.pQueuePriorities = &priority;
		queueInfos.push_back(q);
		if (presentFamily_ != graphicsFamily_)
		{
			VkDeviceQueueCreateInfo q2 = q;
			q2.queueFamilyIndex = presentFamily_;
			queueInfos.push_back(q2);
		}

		const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		ci.queueCreateInfoCount    = (uint32_t)queueInfos.size();
		ci.pQueueCreateInfos       = queueInfos.data();
		ci.enabledExtensionCount   = 1;
		ci.ppEnabledExtensionNames = extensions;
		ci.pEnabledFeatures        = nullptr;  /* nothing beyond core 1.0 is needed */

		VkResult r = vkCreateDevice(physicalDevice_, &ci, nullptr, &device_);
		if (r != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateDevice failed");

		volkLoadDevice(device_);
		vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
		vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
	}

	void CreateCommandPool()
	{
		VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		ci.queueFamilyIndex = graphicsFamily_;
		if (vkCreateCommandPool(device_, &ci, nullptr, &commandPool_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateCommandPool failed");
	}

	/* ── Swapchain ─────────────────────────────────────────────────────────── */

	static VkSurfaceFormatKHR PickSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
	{
		/* Non-sRGB B8G8R8A8 first: it keeps the 8-bit code values the OpenGL
		 * renderer produced (no linear->sRGB conversion on write). */
		const VkFormat preferred[] = {
			VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_FORMAT_B8G8R8A8_SRGB,
			VK_FORMAT_R8G8B8A8_SRGB,
		};
		for (VkFormat f : preferred)
			for (const auto& s : formats)
				if (s.format == f && s.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					return s;
		for (VkFormat f : preferred)
			for (const auto& s : formats)
				if (s.format == f)
					return s;
		return formats[0];
	}

	bool CreateSwapchain(uint32_t width, uint32_t height, VkSwapchainKHR old)
	{
		VkSurfaceCapabilitiesKHR caps{};
		if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps) != VK_SUCCESS)
			return false;

		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
		if (formatCount == 0) return false;
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

		uint32_t modeCount = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
		std::vector<VkPresentModeKHR> modes(modeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, modes.data());

		/* FIFO is required by the spec to be supported, so it is not a
		 * preference: it is the frame pacer. */
		bool haveFifo = false;
		for (VkPresentModeKHR m : modes)
			if (m == VK_PRESENT_MODE_FIFO_KHR) haveFifo = true;
		if (!haveFifo)
			throw std::runtime_error("Vulkan: surface does not support VK_PRESENT_MODE_FIFO_KHR");

		const VkSurfaceFormatKHR surfaceFormat = PickSurfaceFormat(formats);

		VkExtent2D extent{};
		if (caps.currentExtent.width != UINT32_MAX)
			extent = caps.currentExtent;
		else
		{
			extent.width  = std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
			extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
		}
		if (extent.width == 0 || extent.height == 0) return false;

		minImageCount_ = std::max(2u, caps.minImageCount);
		if (caps.maxImageCount != 0 && minImageCount_ > caps.maxImageCount)
			minImageCount_ = caps.maxImageCount;

		VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
		{
			const VkCompositeAlphaFlagBitsKHR any[] = {
				VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
				VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
				VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
			};
			for (auto a : any)
				if (caps.supportedCompositeAlpha & a) { composite = a; break; }
		}

		const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if ((caps.supportedUsageFlags & usage) != usage)
			throw std::runtime_error("Vulkan: surface does not support COLOR_ATTACHMENT|TRANSFER_DST");

		VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		ci.surface          = surface_;
		ci.minImageCount    = minImageCount_;
		ci.imageFormat      = surfaceFormat.format;
		ci.imageColorSpace  = surfaceFormat.colorSpace;
		ci.imageExtent      = extent;
		ci.imageArrayLayers = 1;
		ci.imageUsage       = usage;
		const uint32_t familyIndices[2] = { graphicsFamily_, presentFamily_ };
		if (graphicsFamily_ != presentFamily_)
		{
			/* Two families: the swapchain image must be visible to both queues. */
			ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
			ci.queueFamilyIndexCount = 2;
			ci.pQueueFamilyIndices   = familyIndices;
		}
		else
		{
			ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}
		ci.preTransform     = caps.currentTransform;
		ci.compositeAlpha   = composite;
		ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
		ci.clipped          = VK_TRUE;
		ci.oldSwapchain     = old;

		VkSwapchainKHR created = VK_NULL_HANDLE;
		VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &created);
		if (r != VK_SUCCESS)
			return false;

		DestroySwapchainImages();
		if (old != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(device_, old, nullptr);

		swapchain_       = created;
		swapchainFormat_ = surfaceFormat.format;
		extent_          = extent;
		presentMode_     = VK_PRESENT_MODE_FIFO_KHR;

		uint32_t imageCount = 0;
		vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
		swapImages_.resize(imageCount);
		vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapImages_.data());

		CreateSyncObjects();

		std::fprintf(stderr,
			"Vulkan: %s | format %d | present %s | minImageCount %u | images %u | extent %ux%u\n",
			deviceName_, (int)swapchainFormat_, PresentModeName(),
			minImageCount_, imageCount, extent.width, extent.height);
		std::fflush(stderr);

		return true;
	}

	void CreateSyncObjects()
	{
		const uint32_t n = (uint32_t)swapImages_.size();
		acquireSemaphores_.assign(n, VK_NULL_HANDLE);
		renderFinishedSemaphores_.assign(n, VK_NULL_HANDLE);
		fences_.assign(n, VK_NULL_HANDLE);

		VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (uint32_t i = 0; i < n; ++i)
		{
			if (vkCreateSemaphore(device_, &si, nullptr, &acquireSemaphores_[i]) != VK_SUCCESS ||
			    vkCreateSemaphore(device_, &si, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
			    vkCreateFence(device_, &fi, nullptr, &fences_[i]) != VK_SUCCESS)
				throw std::runtime_error("Vulkan: failed to create frame synchronisation objects");
		}
	}

	void DestroySwapchainImages()
	{
		for (uint32_t i = 0; i < acquireSemaphores_.size(); ++i)
		{
			if (acquireSemaphores_[i] != VK_NULL_HANDLE)
				vkDestroySemaphore(device_, acquireSemaphores_[i], nullptr);
			if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE)
				vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
			if (fences_[i] != VK_NULL_HANDLE)
				vkDestroyFence(device_, fences_[i], nullptr);
		}
		acquireSemaphores_.clear();
		renderFinishedSemaphores_.clear();
		fences_.clear();
		/* Swapchain images are owned by the swapchain itself. */
		swapImages_.clear();
	}

	void DestroySwapchain()
	{
		if (device_ == VK_NULL_HANDLE) return;
		DestroySwapchainImages();
		if (swapchain_ != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(device_, swapchain_, nullptr);
			swapchain_ = VK_NULL_HANDLE;
		}
	}

private:
	VkInstance         instance_       = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
	VkSurfaceKHR       surface_        = VK_NULL_HANDLE;
	VkPhysicalDevice   physicalDevice_ = VK_NULL_HANDLE;
	VkDevice           device_         = VK_NULL_HANDLE;
	VkQueue            graphicsQueue_  = VK_NULL_HANDLE;
	VkQueue            presentQueue_   = VK_NULL_HANDLE;
	uint32_t           graphicsFamily_ = 0;
	uint32_t           presentFamily_  = 0;
	char               deviceName_[256] = {0};
	VkCommandPool      commandPool_    = VK_NULL_HANDLE;

	VkSwapchainKHR     swapchain_       = VK_NULL_HANDLE;
	VkFormat           swapchainFormat_ = VK_FORMAT_UNDEFINED;
	VkExtent2D         extent_{};
	uint32_t           minImageCount_   = 2;
	VkPresentModeKHR   presentMode_     = VK_PRESENT_MODE_FIFO_KHR;
	std::vector<VkImage> swapImages_;

	std::vector<VkSemaphore> acquireSemaphores_;
	std::vector<VkSemaphore> renderFinishedSemaphores_;
	std::vector<VkFence>     fences_;

	/* First frames only: lets the trace show where the very first present spends
	 * its time without flooding the log afterwards. */
	uint32_t traceFrames_ = 0;
	uint32_t presentCount_ = 0;
	std::chrono::steady_clock::time_point lastPresentEnd_{};
};
