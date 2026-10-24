// GPU path on the D3D11 RHI (Windows, -d3d11): Skia Ganesh on a Direct3D 12
// device the library creates on the adapter of the RHI's ID3D11Device
// (jsui_gpu_create_d3d11). Each render draws into a shared texture of the
// library's and copies it into the view's render target with the device's
// immediate context, behind a shared fence the immediate context waits on.
//
// Thread and ordering: the immediate context belongs to the thread that
// executes the RHI's command lists, so the job is a command list lambda
// (EnqueueLambda), which runs there in order with the RHI's own commands:
// after what was recorded before it and before this frame's Slate draw. The
// copy is queued on the immediate context like any CopyResource, and D3D11
// orders it with the draws around it. D3D11 has no resource states to keep in
// step.
#include "JsuiHost.h"

#if JSUI_UE_D3D11

#include "ID3D11DynamicRHI.h"
#include "RHICommandList.h"

void FJsuiHost::InitD3D11(FRHICommandListImmediate& RHICmdList)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	RHICmdList.EnqueueLambda(TEXT("JsuiCreateD3D11"),
		[Self](auto&) { Self->CreateJob([] { return jsui_gpu_create_d3d11(GetID3D11DynamicRHI()->RHIGetDevice()); }); });
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
}

void FJsuiHost::RenderD3D11(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	FTextureRHIRef Keep = Texture;
	RHICmdList.EnqueueLambda(TEXT("JsuiRenderD3D11"),
		[Self, Keep, Width, Height, bRepaint](auto&)
		{
			// A 2D texture's resource is its ID3D11Texture2D.
			ID3D11Resource* Resource = GetID3D11DynamicRHI()->RHIGetResource(Keep.GetReference());
			JsuiGpuTarget Target{};
			Target.texture = static_cast<ID3D11Texture2D*>(Resource);
			Target.format = 0;  // the texture's own
			Target.width = Width;
			Target.height = Height;
			Self->RenderJob(Target, bRepaint);
		});
	// Run it now rather than at the frame's next flush (the game thread waits
	// for it before its next runtime call).
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
}

void FJsuiHost::ReleaseD3D11(FRHICommandListImmediate& RHICmdList, TFunction<void()>&& InJob)
{
	RHICmdList.EnqueueLambda(TEXT("JsuiReleaseD3D11"), [Job = MoveTemp(InJob)](auto&) { Job(); });
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
}

#endif  // JSUI_UE_D3D11
