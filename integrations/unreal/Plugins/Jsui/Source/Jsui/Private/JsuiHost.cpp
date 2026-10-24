#include "JsuiHost.h"

#include "JsuiLog.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"

#if JSUI_UE_METAL
#include "IMetalDynamicRHI.h"
#endif

// GPU path. Skia Graphite draws into the render target on the Metal RHI's own
// MTLDevice and MTLCommandQueue, so no copy and no cross-queue sync:
//
//  - SJsuiView::Tick (game thread, Slate's tick, after the world ticked and
//    before Slate paints) queues a render command;
//  - on the render thread it hands a job to IMetalDynamicRHI::RHIRunOnQueue,
//    which the RHI runs on its submission side with the direct queue, in
//    order with the command buffers of every RHI command list submitted
//    before it;
//  - the job creates the Graphite context on that queue (first time) and
//    renders; Graphite commits its command buffer to the queue before
//    jsui_render_gpu_target returns.
//
// Ordering: this frame's Slate draw, which samples the render target, is
// queued on the render thread after our command, so its command buffers are
// committed to the same queue after Graphite's, and Metal's hazard tracking
// (the RHI's textures are tracked on Apple GPUs) makes the GPU run them in
// that order. The RHI has no native graphics queue getter on Metal
// (RHIGetNativeGraphicsQueue is null there), so RHIRunOnQueue is the one way
// onto its queue.
//
// The Graphite context is shared by every view and lives as long as the
// process (the RHI's device does too); only the jobs touch it, and they run
// one at a time.
namespace
{
JsuiGpuContext* GGpu = nullptr;
bool GGpuUnavailable = false;  // context creation failed on this device
}  // namespace

TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> FJsuiHost::Create(int32 Width, int32 Height, float PixelRatio, bool bDevMode)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Host(new FJsuiHost());
	JsuiConfig Cfg;
	jsui_config_init(&Cfg);
	Cfg.width = FMath::Max(1, Width);
	Cfg.height = FMath::Max(1, Height);
	Cfg.pixel_ratio = PixelRatio;
	Cfg.dev_mode = bDevMode ? 1 : 0;
	// Called inside jsui_tick / jsui_send_* on the game thread (and maybe a
	// log line from a render job): queued, drained by SJsuiView after the tick.
	Cfg.message_user = Host.Get();
	Cfg.on_message = [](void* User, const char* Name, const char* Json)
	{
		FJsuiHost* Self = static_cast<FJsuiHost*>(User);
		std::lock_guard<std::mutex> Lock(Self->QueueMutex);
		Self->Messages.Add({UTF8_TO_TCHAR(Name ? Name : ""), UTF8_TO_TCHAR(Json ? Json : "null")});
	};
	Cfg.log_user = Host.Get();
	Cfg.log = [](void* User, JsuiLogLevel Level, const char* Text)
	{
		FJsuiHost* Self = static_cast<FJsuiHost*>(User);
		std::lock_guard<std::mutex> Lock(Self->QueueMutex);
		if (Self->Logs.Num() < 1000) Self->Logs.Add({int32(Level), UTF8_TO_TCHAR(Text ? Text : "")});
	};
	Host->Rt = jsui_create(&Cfg);
	if (!Host->Rt) return nullptr;
	return Host;
}

FJsuiHost::~FJsuiHost()
{
	// Shutdown() normally ran on the game thread already.
	if (Rt) jsui_destroy(Rt);
}

JsuiRuntime* FJsuiHost::Runtime()
{
	WaitRender();
	return Rt;
}

void FJsuiHost::WaitRender()
{
	std::unique_lock<std::mutex> Lock(JobMutex);
	if (Done >= Queued) return;
	if (!JobDone.wait_for(Lock, std::chrono::seconds(2), [this] { return Done >= Queued; }))
	{
		UE_LOG(LogJsui, Warning, TEXT("a render job did not run within 2 s; going on without it"));
		Done = Queued;
	}
}

void FJsuiHost::Shutdown()
{
	WaitRender();
	if (Rt) jsui_destroy(Rt);
	Rt = nullptr;
}

void FJsuiHost::TakeMessages(TArray<FMessage>& Out)
{
	std::lock_guard<std::mutex> Lock(QueueMutex);
	Out = MoveTemp(Messages);
	Messages.Reset();
}

void FJsuiHost::TakeLogs(TArray<FLog>& Out)
{
	std::lock_guard<std::mutex> Lock(QueueMutex);
	Out = MoveTemp(Logs);
	Logs.Reset();
}

// ------------------------------------------------------------ GPU path

bool FJsuiHost::GpuSupported()
{
#if JSUI_UE_METAL
	return IsRHIMetal() && !GGpuUnavailable;
#else
	return false;
#endif
}

bool FJsuiHost::GpuFailed()
{
	WaitRender();
	return bGpuFailed;
}

void FJsuiHost::QueueGpuRender(FTextureRenderTargetResource* Target, int32 Width, int32 Height, bool bRepaint)
{
#if JSUI_UE_METAL
	{
		std::lock_guard<std::mutex> Lock(JobMutex);
		Queued++;
	}
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	ENQUEUE_RENDER_COMMAND(JsuiRender)(
		[Self, Target, Width, Height, bRepaint](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Target ? Target->TextureRHI.GetReference() : nullptr;
			void* Native = Texture ? Texture->GetNativeResource() : nullptr;
			if (!Native)
			{
				Self->FinishJob();
				return;
			}
			if (bRepaint)
			{
				// A new render target: the RHI's initial clear of it is in a
				// command list that may not be submitted yet. Submit it first,
				// or it would land after Graphite's frame and wipe it.
				RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			}
			void* Device = GDynamicRHI->RHIGetNativeDevice();
			GetIMetalDynamicRHI()->RHIRunOnQueue(
				[Self, Device, Native, Width, Height, bRepaint](MTL::CommandQueue* Queue)
				{ Self->RenderJob(Device, Queue, Native, Width, Height, bRepaint); },
				false);
		});
#endif
}

void FJsuiHost::RenderJob(void* Device, void* Queue, void* Texture, int32 Width, int32 Height, bool bRepaint)
{
#if JSUI_UE_METAL
	if (Rt && !bGpuFailed)
	{
		if (!GGpu && !GGpuUnavailable)
		{
			GGpu = jsui_gpu_create_metal(Device, Queue);
			if (GGpu)
			{
				UE_LOG(LogJsui, Log, TEXT("GPU: Skia Graphite on the Metal RHI's device (%s)"),
				       UTF8_TO_TCHAR(static_cast<MTL::Device*>(Device)->name()->utf8String()));
			}
			else
			{
				GGpuUnavailable = true;
			}
		}
		if (!GGpu)
		{
			bGpuFailed = true;
		}
		else
		{
			JsuiGpuTarget Target{};
			Target.texture = Texture;
			Target.format = unsigned(static_cast<MTL::Texture*>(Texture)->pixelFormat());
			Target.width = Width;
			Target.height = Height;
			Target.flags = (!bRepaint && HeldTexture == Texture) ? JSUI_GPU_INCREMENTAL : 0;
			JsuiRect Dirty;
			if (jsui_render_gpu_target(Rt, GGpu, &Target, &Dirty) == 0)
			{
				HeldTexture = Texture;
			}
			else
			{
				HeldTexture = nullptr;
				bGpuFailed = true;
			}
		}
	}
#endif
	FinishJob();
}

void FJsuiHost::FinishJob()
{
	std::lock_guard<std::mutex> Lock(JobMutex);
	Done++;
	JobDone.notify_all();
}

// ------------------------------------------------------------ CPU path

bool FJsuiHost::RenderCpu(int32 Width, int32 Height, bool bForce, JsuiRect& Dirty)
{
	if (!Rt || Width <= 0 || Height <= 0) return false;
	if (Width != CpuWidth || Height != CpuHeight)
	{
		CpuWidth = Width;
		CpuHeight = Height;
		CpuPixels.SetNumZeroed(Width * Height * 4);
		bCpuFresh = true;
	}
	const size_t RowBytes = size_t(Width) * 4;
	if (bCpuFresh || bForce)
	{
		jsui_render_rgba(Rt, CpuPixels.GetData(), Width, Height, RowBytes);
		Dirty = {0, 0, Width, Height};
		bCpuFresh = false;
		return true;
	}
	if (!jsui_needs_redraw(Rt)) return false;
	if (!jsui_render_rgba_incremental(Rt, CpuPixels.GetData(), Width, Height, RowBytes, &Dirty)) return false;
	return Dirty.width > 0 && Dirty.height > 0;
}
