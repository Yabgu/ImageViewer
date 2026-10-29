module;

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <stdexcept>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

/* Generated SPIR-V (see that file for the GLSL and the exact glslc command). */
#include "VulkanShaders.hpp"

export module VulkanRenderer;

import VulkanContext;

/* Canvas background: mid-gray #808080, the same value the OpenGL renderer used
 * for glClearColor. */
constexpr float kCanvasBg[3] = {
	128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f
};

/* Quad placement in Vulkan NDC (x right, y down) plus the sampler choice. */
export struct ImageDrawParams
{
	float posX  = 0.0f;    /* image top-left, NDC                     */
	float posY  = 0.0f;
	float sizeX = 0.0f;    /* image extent, NDC units                 */
	float sizeY = 0.0f;
	bool  nearest = false; /* zoom >= 100 %: exact pixels, no filter  */
};

/* Push-constant block shared with the vertex shader (8 floats = 32 bytes). */
struct ImagePushConstants
{
	float posX, posY, sizeX, sizeY;
	float uv0x, uv0y, uv1x, uv1y;
};

/*
 * VulkanRenderer — everything that draws.
 *
 * Two offscreen images (fbo_[0], fbo_[1]) at the swapchain extent hold phase A
 * and phase B of the temporal dither.  When the filter plugin produced two
 * frames (a >8-bit source), fbo_[0] is drawn from the round-down frame and
 * fbo_[1] from the round-up frame; the loop alternates which FBO is copied into
 * the acquired swapchain image.  When the source already fits the display (one
 * frame) both FBOs are drawn from the same texture and the alternation is
 * visually static.
 *
 * Idle work per frame is exactly one pre-recorded vkCmdCopyImage into the
 * acquired swapchain image (see RecordIdleCommands).  Rendering happens only
 * when Window marks the frame dirty, and then only twice (once per FBO).
 */
export class VulkanRenderer
{
public:
	VulkanRenderer() = default;
	~VulkanRenderer() { Shutdown(); }
	VulkanRenderer(const VulkanRenderer&)            = delete;
	VulkanRenderer& operator=(const VulkanRenderer&) = delete;

	void Initialize(VulkanContext& ctx)
	{
		ctx_ = &ctx;
		device_ = ctx.Device();

		CreateRenderPass();
		CreateSamplers();
		/* The descriptor-set layout has to exist before the pipeline layout that
		 * references it, so this must come before CreatePipelines(). */
		CreateDescriptorSets();
		CreatePipelines();
		CreateReadbackBuffer();
		OnSwapchainChanged();

		/* Pay the driver's first-present cost here, while the window is still
		 * hidden and before any image exists (see WarmUpPresent). */
		WarmUpPresent();
	}

	void Shutdown()
	{
		if (device_ == VK_NULL_HANDLE) return;
		vkDeviceWaitIdle(device_);

		FreeCommandBuffers();

		for (int i = 0; i < 2; ++i)
		{
			if (fbo_[i].framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(device_, fbo_[i].framebuffer, nullptr);
			if (fbo_[i].view != VK_NULL_HANDLE)
				vkDestroyImageView(device_, fbo_[i].view, nullptr);
			if (fbo_[i].image != VK_NULL_HANDLE)
				vkDestroyImage(device_, fbo_[i].image, nullptr);
			if (fbo_[i].memory != VK_NULL_HANDLE)
				vkFreeMemory(device_, fbo_[i].memory, nullptr);
			fbo_[i] = Fbo{};
		}

		DestroyTextures();
		DestroyStaging();

		if (readbackBuffer_ != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device_, readbackBuffer_, nullptr);
			readbackBuffer_ = VK_NULL_HANDLE;
		}
		if (readbackMemory_ != VK_NULL_HANDLE)
		{
			if (readbackPtr_) { vkUnmapMemory(device_, readbackMemory_); readbackPtr_ = nullptr; }
			vkFreeMemory(device_, readbackMemory_, nullptr);
			readbackMemory_ = VK_NULL_HANDLE;
		}

		if (descriptorPool_ != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
			descriptorPool_ = VK_NULL_HANDLE;
		}
		if (descriptorSetLayout_ != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
			descriptorSetLayout_ = VK_NULL_HANDLE;
		}
		if (pipeline_ != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device_, pipeline_, nullptr);
			pipeline_ = VK_NULL_HANDLE;
		}
		if (pipelineLayout_ != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
			pipelineLayout_ = VK_NULL_HANDLE;
		}
		if (renderPass_ != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(device_, renderPass_, nullptr);
			renderPass_ = VK_NULL_HANDLE;
		}
		for (VkSampler* s : { &samplerNearest_, &samplerLinear_ })
		{
			if (*s != VK_NULL_HANDLE) { vkDestroySampler(device_, *s, nullptr); *s = VK_NULL_HANDLE; }
		}

		device_ = VK_NULL_HANDLE;
	}

	/* ── Swapchain lifecycle ───────────────────────────────────────────────── */

	/* Called by Window after every successful swapchain (re)creation — first
	 * creation included — with the device already idle.  This is the only place
	 * that (re)records the idle command buffers, so an OUT_OF_DATE / SUBOPTIMAL
	 * / resize recreation cannot leave them pointing at destroyed images. */
	void OnSwapchainChanged()
	{
		const VkExtent2D extent = ctx_->Extent();
		if (extent.width == 0 || extent.height == 0) return;

		FreeCommandBuffers();
		DestroyFbos();
		CreateFbos(extent);
		AllocateCommandBuffers();
		RecordIdleCommands();
	}

	VkRenderPass RenderPass() const noexcept { return renderPass_; }
	uint32_t     ImageCount() const noexcept { return (uint32_t)idleCmd_.size() / 2u; }
	bool         HasImage()   const noexcept { return tex_[0].view != VK_NULL_HANDLE; }
	bool         HasTwoPhases() const noexcept { return tex_[1].view != VK_NULL_HANDLE; }

	/* Startup-only warm-up: acquire one swapchain image, clear it to the canvas
	 * colour, present it and wait for the device.  The driver's first FIFO
	 * present is expensive on some systems (measured: 1.6–4.8 s on this
	 * hybrid-GPU laptop); doing it here — window hidden, no image yet — moves
	 * that cost off the first frame that actually carries the image.  Called
	 * once from Initialize(); never on swapchain recreation, so a visible
	 * window never flashes the clear colour. */
	void WarmUpPresent()
	{
		uint32_t imageIndex = 0;
		if (!ctx_->AcquireNextImage(0, &imageIndex))
		{
			IviewTrace("warm-up present skipped (acquire failed)");
			return;
		}
		IviewTrace("warm-up present: acquired image, submitting clear");

		VkCommandBuffer cmd = BeginOneShot();
		const VkImage dst = ctx_->Image(imageIndex);

		VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image               = dst;
		toDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &toDst);

		VkClearColorValue clear{};
		clear.float32[0] = kCanvasBg[0];
		clear.float32[1] = kCanvasBg[1];
		clear.float32[2] = kCanvasBg[2];
		clear.float32[3] = 1.0f;
		const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

		VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.image               = dst;
		toPresent.subresourceRange    = range;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &toPresent);

				EndCommand(cmd);
		const auto tSubmit    = std::chrono::steady_clock::now();
		const bool presented  = ctx_->SubmitAndPresent(0, imageIndex, cmd);
		const auto tPresented = std::chrono::steady_clock::now();
		ctx_->QueueWaitIdle();
		const auto tIdle      = std::chrono::steady_clock::now();
		vkFreeCommandBuffers(device_, ctx_->CommandPool(), 1, &cmd);

		if (IviewTraceEnabled())
		{
			const double presentMs =
				std::chrono::duration<double, std::milli>(tPresented - tSubmit).count();
			const double idleMs =
				std::chrono::duration<double, std::milli>(tIdle - tPresented).count();
			std::fprintf(stderr,
				"IVIEW_TRACE warm-up (hidden): present %.2f ms, queue-wait-idle %.2f ms%s\n",
				presentMs, idleMs, presented ? "" : " [not presented]");
			std::fflush(stderr);
		}
		IviewTrace(presented ? "warm-up present done (hidden window)"
		                     : "warm-up present returned not-presented");
	}

	/* ── Texture upload ────────────────────────────────────────────────────── */

	/* Uploads tightly packed RGBA8 frames (stride == width * 4) of
	 * `width` x `height` pixels.  `rgba1` may be null for an 8-bit source, in
	 * which case both phases sample the same texture.  Waits for the device, so
	 * it is only called on (re)load, never per frame. */
	void Upload(uint32_t width, uint32_t height, const uint8_t* rgba0, const uint8_t* rgba1)
	{
		if (width == 0 || height == 0 || rgba0 == nullptr)
			throw std::runtime_error("Vulkan: upload called with an empty image");

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(ctx_->PhysicalDevice(), &props);
		if (width > props.limits.maxImageDimension2D || height > props.limits.maxImageDimension2D)
			throw std::runtime_error(
				"image is larger than this GPU's maximum texture size (" +
				std::to_string(props.limits.maxImageDimension2D) + " px)");

		ctx_->WaitIdle();
		DestroyTextures();

		const VkDeviceSize bytes = VkDeviceSize(width) * height * 4u;
		const VkDeviceSize stagingBefore = stagingSize_;
		EnsureStaging(bytes);
		const bool stagingReused = (stagingBefore >= bytes && stagingBefore != 0);

		std::memcpy(stagingPtr_, rgba0, (size_t)bytes);
		CreateTextureImage(tex_[0], width, height);

		if (rgba1 != nullptr && rgba1 != rgba0)
		{
			std::memcpy(stagingPtr_, rgba1, (size_t)bytes);
			CreateTextureImage(tex_[1], width, height);
		}

		WriteDescriptors();

		if (IviewTraceEnabled())
		{
			const unsigned phases = (rgba1 != nullptr && rgba1 != rgba0) ? 2u : 1u;
			std::fprintf(stderr,
				"VK_UPLOAD: %ux%u frame_bytes=%llu phases=%u host_to_staging=%llu "
				"staging_to_image=%llu staging_alloc=%llu staging_reused=%s "
				"submits=%u device_waits=%u one_shot_cmds=%u\n",
				width, height, (unsigned long long)bytes, phases,
				(unsigned long long)(bytes * phases), (unsigned long long)(bytes * phases),
				(unsigned long long)stagingSize_, stagingReused ? "yes" : "no",
				phases, phases, phases);
			std::fflush(stderr);
		}
	}

	/* ── Recording ─────────────────────────────────────────────────────────── */

	/* Begins the image pass on one FBO: clear, viewport, image quad.  ImGui draw
	 * data (if any) is recorded by the caller between BeginImagePass and
	 * EndImagePass so that both phases receive identical chrome. */
	void BeginImagePass(VkCommandBuffer cmd, uint32_t phase, const ImageDrawParams& p)
	{
		const Fbo& f = fbo_[phase & 1u];
		const VkExtent2D extent = ctx_->Extent();

		VkClearValue clear{};
		clear.color.float32[0] = kCanvasBg[0];
		clear.color.float32[1] = kCanvasBg[1];
		clear.color.float32[2] = kCanvasBg[2];
		clear.color.float32[3] = 1.0f;

		VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rp.renderPass      = renderPass_;
		rp.framebuffer     = f.framebuffer;
		rp.renderArea      = { {0, 0}, extent };
		rp.clearValueCount = 1;
		rp.pClearValues    = &clear;
		vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport vp{};
		vp.width    = (float)extent.width;
		vp.height   = (float)extent.height;
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &vp);

		const VkRect2D scissor{ {0, 0}, extent };
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		if (!HasImage()) return;   /* clear only */

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
		const uint32_t slot = (phase & 1u);
		const VkDescriptorSet set = p.nearest ? descNearest_[slot] : descLinear_[slot];
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
		                        0, 1, &set, 0, nullptr);

		ImagePushConstants pc{};
		pc.posX  = p.posX;  pc.posY  = p.posY;
		pc.sizeX = p.sizeX; pc.sizeY = p.sizeY;
		pc.uv0x  = 0.0f;    pc.uv0y  = 0.0f;
		pc.uv1x  = 1.0f;    pc.uv1y  = 1.0f;
		vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
		                   0, sizeof(pc), &pc);

		vkCmdDraw(cmd, 4, 1, 0, 0);
	}

	void EndImagePass(VkCommandBuffer cmd) noexcept
	{
		vkCmdEndRenderPass(cmd);
	}

	/* Barrier + copy of `phase`'s FBO into the acquired swapchain image, then
	 * the transition back to PRESENT_SRC_KHR.  This is the whole of a frame's
	 * work when nothing is dirty. */
	void CopyPhaseToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex, uint32_t phase)
	{
		const Fbo& f = fbo_[phase & 1u];
		const VkImage dst = ctx_->Image(imageIndex);
		const VkExtent2D extent = ctx_->Extent();

		/* Order this read after the render pass that wrote the FBO (same submit)
		 * and after the copy in the previous submission (cross-submit RAW). */
		VkImageMemoryBarrier fboBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		fboBarrier.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		fboBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
		fboBarrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		fboBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		fboBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		fboBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		fboBarrier.image               = f.image;
		fboBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &fboBarrier);

		VkImageMemoryBarrier swapToDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		swapToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		swapToDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		swapToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		swapToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		swapToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		swapToDst.image               = dst;
		swapToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &swapToDst);

		VkImageCopy region{};
		region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.extent         = { extent.width, extent.height, 1 };
		vkCmdCopyImage(cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.image               = dst;
		toPresent.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, nullptr, 0, nullptr, 1, &toPresent);
	}

	/* Pre-recorded idle buffer: the acquire has already happened, so the buffer
	 * carries only the swapchain layout dance and one vkCmdCopyImage.  It is
	 * recorded exactly once per swapchain generation, in RecordIdleCommands(). */
	VkCommandBuffer IdleCommand(uint32_t imageIndex, uint32_t phase) const
	{
		return idleCmd_[(size_t)imageIndex * 2u + (phase & 1u)];
	}

	VkCommandBuffer DirtyCommand(uint32_t slot) const { return dirtyCmd_[slot]; }

	/* `reusable` marks the pre-recorded idle buffers: they are recorded once and
	 * submitted on every frame, so they must not carry ONE_TIME_SUBMIT (and they
	 * are never reset).  Everything else is recorded, submitted once and reset on
	 * the next recording. */
	void BeginCommand(VkCommandBuffer cmd, bool reusable = false) const
	{
		if (!reusable)
			vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = reusable ? 0u : (VkCommandBufferUsageFlags)VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkBeginCommandBuffer failed");
	}

	void EndCommand(VkCommandBuffer cmd) const
	{
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkEndCommandBuffer failed");
	}

	/* ── Diagnostics ───────────────────────────────────────────────────────── */

	/* Step-7 check: copy each FBO into a swapchain image 30 times and read one
	 * pixel back after every copy.  Prints the result to stderr; returns false
	 * if the pixel ever changed (which would mean the FBO was recreated or
	 * overwritten when it should not have been). */
	bool SelfTestFboPreservation()
	{
		if (!HasImage() || readbackPtr_ == nullptr)
		{
			std::fprintf(stderr, "VK_SELFTEST: no image uploaded, skipped\n");
			return false;
		}

		bool ok = true;
		for (uint32_t phase = 0; phase < 2; ++phase)
		{
			uint8_t first[4] = {0, 0, 0, 0};
			bool phaseOk = true;
			for (int i = 0; i < 30; ++i)
			{
				const uint32_t slot = (uint32_t)i % ctx_->ImageCount();
				uint32_t imageIndex = 0;
				if (!ctx_->AcquireNextImage(slot, &imageIndex))
				{
					std::fprintf(stderr, "VK_SELFTEST: acquire failed, aborted\n");
					return false;
				}

				VkCommandBuffer cmd = BeginOneShot();
				CopyPhaseToSwapchain(cmd, imageIndex, phase);
				ReadbackCenterInto(cmd, phase);
				EndCommand(cmd);
				const bool presented = ctx_->SubmitAndPresent(slot, imageIndex, cmd);
				vkDeviceWaitIdle(device_);
				vkFreeCommandBuffers(device_, ctx_->CommandPool(), 1, &cmd);
				if (!presented)
				{
					std::fprintf(stderr, "VK_SELFTEST: present failed, aborted\n");
					return false;
				}

				const uint8_t* p = static_cast<const uint8_t*>(readbackPtr_);
				if (i == 0) std::memcpy(first, p, 4);
				else if (std::memcmp(first, p, 4) != 0)
				{
					std::fprintf(stderr,
						"VK_SELFTEST: phase %u FBO changed at copy %d "
						"(%02x%02x%02x%02x -> %02x%02x%02x%02x)\n",
						phase, i, first[0], first[1], first[2], first[3],
						p[0], p[1], p[2], p[3]);
					phaseOk = false;
					ok = false;
					break;
				}
			}
			std::fprintf(stderr,
				"VK_SELFTEST: phase %u centre pixel = %02x %02x %02x %02x, stable over 30 copies: %s\n",
				phase, first[0], first[1], first[2], first[3], phaseOk ? "yes" : "NO");
		}
		return ok;
	}

private:
	struct Fbo
	{
		VkImage        image       = VK_NULL_HANDLE;
		VkDeviceMemory memory      = VK_NULL_HANDLE;
		VkImageView    view        = VK_NULL_HANDLE;
		VkFramebuffer  framebuffer = VK_NULL_HANDLE;
	};

	struct Texture
	{
		VkImage        image  = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView    view   = VK_NULL_HANDLE;
		uint32_t       width  = 0;
		uint32_t       height = 0;
	};

	/* ── Setup helpers ─────────────────────────────────────────────────────── */

	uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
	{
		VkPhysicalDeviceMemoryProperties mem{};
		vkGetPhysicalDeviceMemoryProperties(ctx_->PhysicalDevice(), &mem);
		for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
			if ((typeBits & (1u << i)) &&
			    (mem.memoryTypes[i].propertyFlags & props) == props)
				return i;
		throw std::runtime_error("Vulkan: no suitable memory type");
	}

	void CreateRenderPass()
	{
		VkAttachmentDescription color{};
		color.format         = ctx_->SwapchainFormat();
		color.samples        = VK_SAMPLE_COUNT_1_BIT;
		color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		/* The FBO rests in TRANSFER_SRC_OPTIMAL between frames; the excursion
		 * into COLOR_ATTACHMENT_OPTIMAL exists only for the subpass and is
		 * declared here, never recorded per frame. */
		color.initialLayout  = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		color.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		const VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments    = &ref;

		/* Orders this pass after the copy that read the same FBO in the previous
		 * submission (write-after-read) and after the previous pass's writes. */
		VkSubpassDependency dep{};
		dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
		dep.dstSubpass    = 0;
		dep.srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT |
		                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
		                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		ci.attachmentCount = 1;
		ci.pAttachments    = &color;
		ci.subpassCount    = 1;
		ci.pSubpasses      = &subpass;
		ci.dependencyCount = 1;
		ci.pDependencies   = &dep;
		if (vkCreateRenderPass(device_, &ci, nullptr, &renderPass_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateRenderPass failed");
	}

	VkShaderModule CreateShaderModule(const uint32_t* code, size_t bytes)
	{
		VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = bytes;
		ci.pCode    = code;
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device_, &ci, nullptr, &module) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateShaderModule failed");
		return module;
	}

	void CreatePipelines()
	{
		VkShaderModule vert = CreateShaderModule(IviewShaders::kImageVertSpv,
		                                         IviewShaders::kImageVertSpvBytes);
		VkShaderModule frag = CreateShaderModule(IviewShaders::kImageFragSpv,
		                                         IviewShaders::kImageFragSpvBytes);

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName  = "main";

		/* No vertex buffer: the quad is generated from gl_VertexIndex. */
		VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

		VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;

		VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rast.polygonMode = VK_POLYGON_MODE_FILL;
		rast.cullMode    = VK_CULL_MODE_NONE;
		rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rast.lineWidth   = 1.0f;

		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable    = VK_FALSE;
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		blend.attachmentCount = 1;
		blend.pAttachments    = &blendAttachment;

		const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates    = dynamics;

		VkPushConstantRange push{};
		push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		push.offset     = 0;
		push.size       = sizeof(ImagePushConstants);

		VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		lci.setLayoutCount         = 1;
		lci.pSetLayouts            = &descriptorSetLayout_;
		lci.pushConstantRangeCount = 1;
		lci.pPushConstantRanges    = &push;
		if (vkCreatePipelineLayout(device_, &lci, nullptr, &pipelineLayout_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreatePipelineLayout failed");

		VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		ci.stageCount          = 2;
		ci.pStages             = stages;
		ci.pVertexInputState   = &vertexInput;
		ci.pInputAssemblyState = &ia;
		ci.pViewportState      = &viewport;
		ci.pRasterizationState = &rast;
		ci.pMultisampleState   = &ms;
		ci.pColorBlendState    = &blend;
		ci.pDynamicState       = &dynamic;
		ci.layout              = pipelineLayout_;
		ci.renderPass          = renderPass_;
		ci.subpass             = 0;

		VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_);
		vkDestroyShaderModule(device_, vert, nullptr);
		vkDestroyShaderModule(device_, frag, nullptr);
		if (r != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateGraphicsPipelines failed");
	}

	VkSampler CreateSampler(VkFilter filter)
	{
		VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		ci.magFilter    = filter;
		ci.minFilter    = filter;
		ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		ci.maxLod       = 0.0f;
		VkSampler s = VK_NULL_HANDLE;
		if (vkCreateSampler(device_, &ci, nullptr, &s) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateSampler failed");
		return s;
	}

	void CreateSamplers()
	{
		samplerNearest_ = CreateSampler(VK_FILTER_NEAREST);
		samplerLinear_  = CreateSampler(VK_FILTER_LINEAR);
	}

	void CreateDescriptorSets()
	{
		VkDescriptorSetLayoutBinding binding{};
		binding.binding         = 0;
		binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		lci.bindingCount = 1;
		lci.pBindings    = &binding;
		if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateDescriptorSetLayout failed");

		VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 };
		VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pci.maxSets       = 4;
		pci.poolSizeCount = 1;
		pci.pPoolSizes    = &size;
		if (vkCreateDescriptorPool(device_, &pci, nullptr, &descriptorPool_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateDescriptorPool failed");

		VkDescriptorSetLayout layouts[4] = {
			descriptorSetLayout_, descriptorSetLayout_,
			descriptorSetLayout_, descriptorSetLayout_
		};
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool     = descriptorPool_;
		ai.descriptorSetCount = 4;
		ai.pSetLayouts        = layouts;
		VkDescriptorSet sets[4]{};
		if (vkAllocateDescriptorSets(device_, &ai, sets) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAllocateDescriptorSets failed");
		descNearest_[0] = sets[0];
		descLinear_[0]  = sets[1];
		descNearest_[1] = sets[2];
		descLinear_[1]  = sets[3];
	}

	/* One descriptor set per (phase, sampler).  Phase 1 falls back to phase 0's
	 * view when the source produced a single frame; it is never a copy of the
	 * phase-0 framebuffer. */
	void WriteDescriptors()
	{
		const VkImageView views[2] = {
			tex_[0].view,
			tex_[1].view != VK_NULL_HANDLE ? tex_[1].view : tex_[0].view
		};
		const VkSampler samplers[2] = { samplerNearest_, samplerLinear_ };
		VkDescriptorSet targets[2][2] = {
			{ descNearest_[0], descLinear_[0] },
			{ descNearest_[1], descLinear_[1] },
		};

		for (int phase = 0; phase < 2; ++phase)
		{
			for (int s = 0; s < 2; ++s)
			{
				VkDescriptorImageInfo info{};
				info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				info.imageView   = views[phase];
				info.sampler     = samplers[s];

				VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				write.dstSet          = targets[phase][s];
				write.dstBinding      = 0;
				write.descriptorCount = 1;
				write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				write.pImageInfo      = &info;
				vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
			}
		}
	}

	/* ── FBOs, command buffers ─────────────────────────────────────────────── */

	void DestroyFbos()
	{
		for (int i = 0; i < 2; ++i)
		{
			if (fbo_[i].framebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(device_, fbo_[i].framebuffer, nullptr); fbo_[i].framebuffer = VK_NULL_HANDLE; }
			if (fbo_[i].view        != VK_NULL_HANDLE) { vkDestroyImageView(device_, fbo_[i].view, nullptr);        fbo_[i].view        = VK_NULL_HANDLE; }
			if (fbo_[i].image       != VK_NULL_HANDLE) { vkDestroyImage(device_, fbo_[i].image, nullptr);            fbo_[i].image       = VK_NULL_HANDLE; }
			if (fbo_[i].memory      != VK_NULL_HANDLE) { vkFreeMemory(device_, fbo_[i].memory, nullptr);             fbo_[i].memory      = VK_NULL_HANDLE; }
		}
	}

	void CreateFbos(VkExtent2D extent)
	{
		if (renderPass_ == VK_NULL_HANDLE)
			throw std::runtime_error("Vulkan: FBOs requested before the render pass existed");

		for (int i = 0; i < 2; ++i)
		{
			Fbo& f = fbo_[i];

			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType     = VK_IMAGE_TYPE_2D;
			ici.format        = ctx_->SwapchainFormat();
			ici.extent        = { extent.width, extent.height, 1 };
			ici.mipLevels     = 1;
			ici.arrayLayers   = 1;
			ici.samples       = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
			ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			                    VK_IMAGE_USAGE_SAMPLED_BIT;
			ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
			ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			if (vkCreateImage(device_, &ici, nullptr, &f.image) != VK_SUCCESS)
				throw std::runtime_error("Vulkan: vkCreateImage (FBO) failed");

			VkMemoryRequirements req{};
			vkGetImageMemoryRequirements(device_, f.image, &req);
			VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			ai.allocationSize  = req.size;
			ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (vkAllocateMemory(device_, &ai, nullptr, &f.memory) != VK_SUCCESS)
				throw std::runtime_error("Vulkan: vkAllocateMemory (FBO) failed");
			vkBindImageMemory(device_, f.image, f.memory, 0);

			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image            = f.image;
			vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
			vci.format           = ici.format;
			vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			if (vkCreateImageView(device_, &vci, nullptr, &f.view) != VK_SUCCESS)
				throw std::runtime_error("Vulkan: vkCreateImageView (FBO) failed");

			VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			fci.renderPass      = renderPass_;
			fci.attachmentCount = 1;
			fci.pAttachments    = &f.view;
			fci.width           = extent.width;
			fci.height          = extent.height;
			fci.layers          = 1;
			if (vkCreateFramebuffer(device_, &fci, nullptr, &f.framebuffer) != VK_SUCCESS)
				throw std::runtime_error("Vulkan: vkCreateFramebuffer failed");
		}

		/* One-time: put both FBOs into the layout the render pass declares as its
		 * initial layout, so a dirty frame needs no separate transition. */
		VkCommandBuffer cmd = BeginOneShot();
		for (int i = 0; i < 2; ++i)
		{
			VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image               = fbo_[i].image;
			b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			                     0, 0, nullptr, 0, nullptr, 1, &b);
		}
		SubmitOneShot(cmd);
	}

	void AllocateCommandBuffers()
	{
		const uint32_t images = ctx_->ImageCount();
		std::vector<VkCommandBuffer> buffers(images * 3u);

		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool        = ctx_->CommandPool();
		ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = (uint32_t)buffers.size();
		if (vkAllocateCommandBuffers(device_, &ai, buffers.data()) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAllocateCommandBuffers failed");

		idleCmd_.assign(buffers.begin(), buffers.begin() + images * 2u);
		dirtyCmd_.assign(buffers.begin() + images * 2u, buffers.end());
	}

	void FreeCommandBuffers()
	{
		std::vector<VkCommandBuffer> all;
		all.insert(all.end(), idleCmd_.begin(), idleCmd_.end());
		all.insert(all.end(), dirtyCmd_.begin(), dirtyCmd_.end());
		idleCmd_.clear();
		dirtyCmd_.clear();
		if (!all.empty() && device_ != VK_NULL_HANDLE)
			vkFreeCommandBuffers(device_, ctx_->CommandPool(), (uint32_t)all.size(), all.data());
	}

	/* Recorded once per swapchain generation.  Everything an idle frame does is
	 * baked in here: transition to TRANSFER_DST, one vkCmdCopyImage from the
	 * phase's FBO, transition to PRESENT_SRC_KHR. */
	void RecordIdleCommands()
	{
		const uint32_t images = ctx_->ImageCount();
		for (uint32_t image = 0; image < images; ++image)
		{
			for (uint32_t phase = 0; phase < 2; ++phase)
			{
				VkCommandBuffer cmd = IdleCommand(image, phase);
				BeginCommand(cmd, /*reusable=*/true);
				CopyPhaseToSwapchain(cmd, image, phase);
				EndCommand(cmd);
			}
		}
	}

	/* ── One-shot helpers (upload, FBO init, readback) ─────────────────────── */

	VkCommandBuffer BeginOneShot()
	{
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool        = ctx_->CommandPool();
		ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAllocateCommandBuffers (one-shot) failed");
		BeginCommand(cmd);
		return cmd;
	}

	void SubmitOneShot(VkCommandBuffer cmd)
	{
		EndCommand(cmd);
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &cmd;
		if (vkQueueSubmit(ctx_->GraphicsQueue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkQueueSubmit (one-shot) failed");
		vkDeviceWaitIdle(device_);
		vkFreeCommandBuffers(device_, ctx_->CommandPool(), 1, &cmd);
	}

	/* ── Texture ───────────────────────────────────────────────────────────── */

	void CreateReadbackBuffer()
	{
		VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		ci.size        = 4;
		ci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device_, &ci, nullptr, &readbackBuffer_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateBuffer (readback) failed");

		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(device_, readbackBuffer_, &req);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize  = req.size;
		ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(device_, &ai, nullptr, &readbackMemory_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAllocateMemory (readback) failed");
		vkBindBufferMemory(device_, readbackBuffer_, readbackMemory_, 0);
		vkMapMemory(device_, readbackMemory_, 0, VK_WHOLE_SIZE, 0, &readbackPtr_);
	}

	void DestroyStaging()
	{
		if (stagingBuffer_ != VK_NULL_HANDLE) { vkDestroyBuffer(device_, stagingBuffer_, nullptr); stagingBuffer_ = VK_NULL_HANDLE; }
		if (stagingMemory_ != VK_NULL_HANDLE)
		{
			if (stagingPtr_) { vkUnmapMemory(device_, stagingMemory_); stagingPtr_ = nullptr; }
			vkFreeMemory(device_, stagingMemory_, nullptr);
			stagingMemory_ = VK_NULL_HANDLE;
		}
		stagingSize_ = 0;
	}

	void EnsureStaging(VkDeviceSize bytes)
	{
		if (stagingSize_ >= bytes && stagingPtr_ != nullptr) return;
		DestroyStaging();

		VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		ci.size        = bytes;
		ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device_, &ci, nullptr, &stagingBuffer_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateBuffer (staging) failed");

		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(device_, stagingBuffer_, &req);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize  = req.size;
		ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(device_, &ai, nullptr, &stagingMemory_) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAllocateMemory (staging) failed");
		vkBindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0);
		vkMapMemory(device_, stagingMemory_, 0, VK_WHOLE_SIZE, 0, &stagingPtr_);
		stagingSize_ = bytes;
	}

	/* Creates `tex` and fills it from the staging buffer, which must already
	 * hold the frame.  Leaves the image in SHADER_READ_ONLY_OPTIMAL. */
	void CreateTextureImage(Texture& tex, uint32_t width, uint32_t height)
	{
		VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ci.imageType     = VK_IMAGE_TYPE_2D;
		ci.format        = VK_FORMAT_R8G8B8A8_UNORM;   /* frames are packed RGBA8 */
		ci.extent        = { width, height, 1 };
		ci.mipLevels     = 1;
		ci.arrayLayers   = 1;
		ci.samples       = VK_SAMPLE_COUNT_1_BIT;
		ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
		ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(device_, &ci, nullptr, &tex.image) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateImage (texture) failed");

		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(device_, tex.image, &req);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize  = req.size;
		ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (vkAllocateMemory(device_, &ai, nullptr, &tex.memory) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkAllocateMemory (texture) failed");
		vkBindImageMemory(device_, tex.image, tex.memory, 0);

		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image            = tex.image;
		vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
		vci.format           = ci.format;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(device_, &vci, nullptr, &tex.view) != VK_SUCCESS)
			throw std::runtime_error("Vulkan: vkCreateImageView (texture) failed");

		VkCommandBuffer cmd = BeginOneShot();

		VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image               = tex.image;
		toDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &toDst);

		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent      = { width, height, 1 };
		vkCmdCopyBufferToImage(cmd, stagingBuffer_, tex.image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		toRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toRead.image               = tex.image;
		toRead.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		toRead.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &toRead);

		SubmitOneShot(cmd);   /* waits for the device */

		tex.width  = width;
		tex.height = height;
	}

	void DestroyTexture(Texture& tex)
	{
		if (device_ == VK_NULL_HANDLE) return;
		if (tex.view   != VK_NULL_HANDLE) { vkDestroyImageView(device_, tex.view, nullptr);   tex.view   = VK_NULL_HANDLE; }
		if (tex.image  != VK_NULL_HANDLE) { vkDestroyImage(device_, tex.image, nullptr);      tex.image  = VK_NULL_HANDLE; }
		if (tex.memory != VK_NULL_HANDLE) { vkFreeMemory(device_, tex.memory, nullptr);       tex.memory = VK_NULL_HANDLE; }
		tex.width = tex.height = 0;
	}

	void DestroyTextures()
	{
		DestroyTexture(tex_[0]);
		DestroyTexture(tex_[1]);
	}

	/* ── Readback ──────────────────────────────────────────────────────────── */

	void ReadbackCenterInto(VkCommandBuffer cmd, uint32_t phase)
	{
		const Fbo& f = fbo_[phase & 1u];
		const VkExtent2D extent = ctx_->Extent();

		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image               = f.image;
		b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		b.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
		b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &b);

		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageOffset      = { (int32_t)(extent.width / 2), (int32_t)(extent.height / 2), 0 };
		region.imageExtent      = { 1, 1, 1 };
		vkCmdCopyImageToBuffer(cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       readbackBuffer_, 1, &region);
	}

private:
	VulkanContext* ctx_    = nullptr;
	VkDevice       device_ = VK_NULL_HANDLE;

	VkRenderPass          renderPass_          = VK_NULL_HANDLE;
	VkPipelineLayout      pipelineLayout_      = VK_NULL_HANDLE;
	VkPipeline            pipeline_            = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
	VkDescriptorPool      descriptorPool_      = VK_NULL_HANDLE;
	VkDescriptorSet       descNearest_[2]      = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkDescriptorSet       descLinear_[2]       = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkSampler             samplerNearest_      = VK_NULL_HANDLE;
	VkSampler             samplerLinear_       = VK_NULL_HANDLE;

	Fbo     fbo_[2];
	Texture tex_[2];

	VkBuffer       stagingBuffer_ = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
	void*          stagingPtr_    = nullptr;
	VkDeviceSize   stagingSize_   = 0;

	VkBuffer       readbackBuffer_ = VK_NULL_HANDLE;
	VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
	void*          readbackPtr_    = nullptr;

	std::vector<VkCommandBuffer> idleCmd_;    /* [image * 2 + phase] */
	std::vector<VkCommandBuffer> dirtyCmd_;   /* [slot]               */
};
