// GPU path on the D3D12 RHI (Windows' default): Skia Ganesh on the RHI's own
// ID3D12Device and direct (graphics) queue, drawing straight into the view's
// render target (a UTextureRenderTarget2D's resource is created with
// D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET).
//
// Resource state. Unreal tracks the render target's state; the RHI moves it
// into ERHIAccess::RTV (D3D12_RESOURCE_STATE_RENDER_TARGET) before the job and
// back to ERHIAccess::SRVMask after it. The render takes it in RENDER_TARGET
// and leaves it there (layout = final_layout), so what Unreal tracks is right
// all along. UE 5.8 uses legacy resource barriers by default
// (r.D3D12.PreferredBarrierImplementation=1, D3D12Adapter.cpp), the same kind
// Ganesh issues. With enhanced barriers (=2, or 0 on a driver that has them),
// ERHIAccess::RTV is D3D12_BARRIER_LAYOUT_RENDER_TARGET, the layout D3D12
// pairs with the legacy RENDER_TARGET state; that mode is untested.
//
// RHIUpdateResourceResidency / RHIFlushResourceBarriers act on the executing
// command list's context (FD3D12CommandContext::Get, D3D12RHI.cpp), hence
// the EnqueueLambda.
//
// RHIRunOnQueue (D3D12RHI.cpp) queues a payload behind every payload
// submitted before it, and the submission thread executes the command lists
// batched so far before it runs the callback (D3D12Submission.cpp:
// FlushBatchedPayloads, Flush() before PreExecute()).
//
// Ordering, per render:
//  1. the render thread records the transition (and makes the resource
//     resident for the command list), then flushes the immediate command list
//     to the GPU (FlushRHIThread: translated and submitted);
//  2. ID3D12DynamicRHI::RHIRunOnQueue(Graphics) runs the job on the RHI's
//     submission side, after those submissions, with the direct queue held;
//     jsui_render_gpu_target executes Ganesh's command lists on that queue
//     before it returns: after the transition in queue order;
//  3. the transition back to SRVMask, and this frame's Slate draw, are
//     recorded after the job was queued, so they execute on the same queue
//     after Ganesh's command lists.
#include "JsuiHost.h"

#if JSUI_UE_D3D12

#include "ID3D12DynamicRHI.h"
#include "RHICommandList.h"

void FJsuiHost::InitD3D12(FRHICommandListImmediate& RHICmdList)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	GetID3D12DynamicRHI()->RHIRunOnQueue(
		ED3D12RHIRunOnQueueType::Graphics,
		[Self](ID3D12CommandQueue* Queue)
		{ Self->CreateJob([Queue] { return jsui_gpu_create_d3d12(GetID3D12DynamicRHI()->RHIGetDevice(0), Queue); }); },
		false);
}

void FJsuiHost::RenderD3D12(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint)
{
	ID3D12DynamicRHI* Rhi = GetID3D12DynamicRHI();
	ID3D12Resource* Resource = Rhi->RHIGetResource(Texture);
	if (!Resource)
	{
		FinishJob();
		return;
	}
	FTextureRHIRef Keep = Texture;  // alive until the job ran

	// 1. Into RENDER_TARGET, submitted before the job.
	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::RTV));
	RHICmdList.EnqueueLambda(TEXT("JsuiD3D12Prepare"),
		[Keep](auto& ExecutingCmdList)
		{
			GetID3D12DynamicRHI()->RHIUpdateResourceResidency(ExecutingCmdList, 0, Keep.GetReference());
			GetID3D12DynamicRHI()->RHIFlushResourceBarriers(ExecutingCmdList, 0);
		});
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

	// 2. The render, on the submission side with the direct queue.
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	Rhi->RHIRunOnQueue(
		ED3D12RHIRunOnQueueType::Graphics,
		[Self, Keep, Resource, Width, Height, bRepaint](ID3D12CommandQueue*)
		{
			JsuiGpuTarget Target{};
			Target.texture = Resource;
			Target.format = 0;  // the resource's own (B8G8R8A8, maybe TYPELESS: drawn as UNORM)
			Target.width = Width;
			Target.height = Height;
			Target.layout = int(D3D12_RESOURCE_STATE_RENDER_TARGET);  // = ERHIAccess::RTV
			Target.final_layout = int(D3D12_RESOURCE_STATE_RENDER_TARGET);
			Self->RenderJob(Target, bRepaint);
		},
		false);

	// 3. Back to what Slate samples, after the job on the same queue.
	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::RTV, ERHIAccess::SRVMask));
}

void FJsuiHost::ReleaseD3D12(FRHICommandListImmediate& RHICmdList, TFunction<void()>&& InJob)
{
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	GetID3D12DynamicRHI()->RHIRunOnQueue(
		ED3D12RHIRunOnQueueType::Graphics, [Job = MoveTemp(InJob)](ID3D12CommandQueue*) { Job(); }, false);
}

#endif  // JSUI_UE_D3D12
