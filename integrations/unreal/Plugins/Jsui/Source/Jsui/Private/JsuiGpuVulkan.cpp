// GPU path on the Vulkan RHI (Linux; Windows with -vulkan): Skia Graphite on
// the RHI's own VkInstance / VkDevice and graphics queue.
//
// Target. Graphite renders only into images with INPUT_ATTACHMENT usage
// (jsui.h), which the image of a UTextureRenderTarget2D does not have. So
// Graphite draws into an image of our own (VulkanTarget: RenderTargetable |
// ShaderResource | InputAttachmentRead, one per view and size), and the RHI
// copies it into the view's render target after each render.
//
// Layouts. VulkanTarget stays in ERHIAccess::CopySrc as far as Unreal knows
// (its initial state, and all the RHI ever does with it is copy out of it),
// i.e. VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: the render takes it in that
// layout (UNDEFINED for a new image: a full repaint) and leaves it there. The
// render target is only touched by the RHI: Unknown -> CopyDest, the copy,
// CopyDest -> SRVMask, so its tracked layout is always right.
//
// Ordering, per render:
//  1. the render thread flushes the immediate command list to the GPU
//     (FlushRHIThread: translated and submitted), so everything recorded
//     before, including VulkanTarget's initial transition and last frame's
//     copy out of it, is submitted;
//  2. IVulkanDynamicRHI::RHIRunOnQueue(Graphics) runs the job on the RHI's
//     submission side, after those submissions, with the graphics queue held
//     (Vulkan queues need external synchronisation). jsui_render_gpu_target
//     submits a full barrier, Graphite's command buffer and another full
//     barrier to that queue before it returns;
//  3. the copy into the render target, recorded on the render thread after
//     the job was queued, is submitted to the same queue after it, behind the
//     render's trailing full barrier; this frame's Slate draw comes later still.
#include "JsuiHost.h"

#if JSUI_UE_VULKAN

#include "IVulkanDynamicRHI.h"
#include "RHICommandList.h"

namespace
{
// Usage flags of VulkanTarget, from its ETextureCreateFlags: the Vulkan RHI
// gives every image TRANSFER_SRC / TRANSFER_DST, RenderTargetable adds
// COLOR_ATTACHMENT, ShaderResource SAMPLED, InputAttachmentRead
// INPUT_ATTACHMENT.
constexpr unsigned kTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;

JsuiGpuContext* CreateVulkanContext(VkQueue Queue)
{
	IVulkanDynamicRHI* Rhi = GetIVulkanDynamicRHI();
	// The device extensions the RHI enabled. It exposes neither its enabled
	// instance extensions nor its VkPhysicalDeviceFeatures2 chain, so none are
	// passed: Skia then assumes no optional features (jsui.h: always valid).
	const TArray<FAnsiString> Extensions = Rhi->RHIGetLoadedDeviceExtensions();
	TArray<const char*> Names;
	for (const FAnsiString& Name : Extensions) Names.Add(*Name);

	JsuiVulkanDevice Device{};
	Device.instance = Rhi->RHIGetVkInstance();
	Device.physical_device = Rhi->RHIGetVkPhysicalDevice();
	Device.device = Rhi->RHIGetVkDevice();
	Device.queue = Queue;
	Device.queue_family = Rhi->RHIGetGraphicsQueueFamilyIndex();
	Device.api_version = Rhi->RHIGetVulkanVersion();
	// The loader the RHI uses (vkGetInstanceProcAddr returns itself).
	Device.get_instance_proc_addr = Rhi->RHIGetVkInstanceProcAddr("vkGetInstanceProcAddr");
	Device.device_extensions = Names.GetData();
	Device.device_extension_count = Names.Num();
	return jsui_gpu_create_vulkan(&Device);
}
}  // namespace

void FJsuiHost::InitVulkan(FRHICommandListImmediate& RHICmdList)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	GetIVulkanDynamicRHI()->RHIRunOnQueue(
		EVulkanRHIRunOnQueueType::Graphics,
		[Self](VkQueue Queue) { Self->CreateJob([Queue] { return CreateVulkanContext(Queue); }); },
		false);
}

void FJsuiHost::RenderVulkan(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint)
{
	IVulkanDynamicRHI* Rhi = GetIVulkanDynamicRHI();
	bool bNew = false;
	if (!VulkanTarget || VulkanTarget->GetSizeXY() != FIntPoint(Width, Height) || VulkanTarget->GetFormat() != Texture->GetFormat())
	{
		const FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::Create2D(TEXT("JsuiGraphiteTarget"), Width, Height, Texture->GetFormat())
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource |
				          ETextureCreateFlags::InputAttachmentRead)
				.SetInitialState(ERHIAccess::CopySrc);
		VulkanTarget = RHICmdList.CreateTexture(Desc);
		bNew = true;
	}
	const VkImage Image = Rhi->RHIGetVkImage(VulkanTarget);
	const VkFormat Format = Rhi->RHIGetViewVkFormat(VulkanTarget);
	if (Image == VK_NULL_HANDLE)
	{
		FinishJob();
		return;
	}

	// 1. Everything recorded so far reaches the queue before the job.
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

	// 2. The render, on the submission side with the graphics queue.
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	FTextureRHIRef Keep = VulkanTarget;  // alive until the job ran
	Rhi->RHIRunOnQueue(
		EVulkanRHIRunOnQueueType::Graphics,
		[Self, Keep, Image, Format, Width, Height, bRepaint, bNew](VkQueue)
		{
			JsuiGpuTarget Target{};
			Target.texture = (void*)Image;
			Target.format = unsigned(Format);
			Target.width = Width;
			Target.height = Height;
			Target.usage = kTargetUsage;
			Target.layout = bNew ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			Target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // = ERHIAccess::CopySrc
			Self->RenderJob(Target, bRepaint || bNew);
		},
		false);

	// 3. Into the view's render target, after the job on the same queue.
	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopyDest));
	RHICmdList.CopyTexture(VulkanTarget, Texture, FRHICopyTextureInfo());
	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
}

void FJsuiHost::ReleaseVulkan(FRHICommandListImmediate& RHICmdList, TFunction<void()>&& InJob)
{
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	GetIVulkanDynamicRHI()->RHIRunOnQueue(
		EVulkanRHIRunOnQueueType::Graphics, [Job = MoveTemp(InJob)](VkQueue) { Job(); }, false);
}

#endif  // JSUI_UE_VULKAN
