// GPU path on the Metal RHI (macOS): Skia Graphite on the RHI's own MTLDevice
// and MTLCommandQueue, drawing straight into the view's render target.
//
// Ordering: the job runs through IMetalDynamicRHI::RHIRunOnQueue, on the
// RHI's submission side with its direct queue, after the command buffers of
// every command list submitted before it. This frame's Slate draw, which
// samples the render target, is committed to the same queue after Graphite's
// command buffer, and Metal's hazard tracking (the RHI's textures are tracked
// on Apple GPUs) makes the GPU run them in that order. There are no layouts
// to keep in step. The Metal RHI has no native graphics queue getter
// (RHIGetNativeGraphicsQueue is null there), so RHIRunOnQueue is the one way
// onto its queue.
#include "JsuiHost.h"

#if JSUI_UE_METAL

#include "IMetalDynamicRHI.h"
#include "RHICommandList.h"

void FJsuiHost::InitMetal(FRHICommandListImmediate& RHICmdList)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	void* Device = GDynamicRHI->RHIGetNativeDevice();
	GetIMetalDynamicRHI()->RHIRunOnQueue(
		[Self, Device](MTL::CommandQueue* Queue) { Self->CreateJob([&] { return jsui_gpu_create_metal(Device, Queue); }); },
		false);
}

void FJsuiHost::RenderMetal(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint)
{
	void* Native = Texture->GetNativeResource();
	if (!Native)
	{
		FinishJob();
		return;
	}
	if (bRepaint)
	{
		// A new render target: the RHI's initial clear of it is in a command
		// list that may not be submitted yet. Submit it first, or it would
		// land after Graphite's frame and wipe it.
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	}
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	GetIMetalDynamicRHI()->RHIRunOnQueue(
		[Self, Native, Width, Height, bRepaint](MTL::CommandQueue*)
		{
			JsuiGpuTarget Target{};
			Target.texture = Native;
			Target.format = unsigned(static_cast<MTL::Texture*>(Native)->pixelFormat());
			Target.width = Width;
			Target.height = Height;
			Self->RenderJob(Target, bRepaint);
		},
		false);
}

#endif  // JSUI_UE_METAL
