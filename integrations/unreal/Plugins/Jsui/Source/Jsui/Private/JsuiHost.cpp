#include "JsuiHost.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "JsuiLog.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "TextureResource.h"

// GPU path. Skia draws the UI straight into the view's render target on the
// RHI's own device, ordered with Unreal's GPU work on the RHI's own queue:
//
//  - SJsuiView::Tick (game thread, Slate's tick, after the world ticked and
//    before Slate paints) queues a render command;
//  - on the render thread, the RHI's path (JsuiGpu<RHI>.cpp) hands a job to
//    the RHI where it submits GPU work: RHIRunOnQueue (Metal, Vulkan, D3D12),
//    which runs it on the RHI's submission side with the graphics queue, in
//    order with the command lists submitted before it; on D3D11 a command
//    list lambda, which runs where the immediate context is used;
//  - the job creates the shared context on that device and queue (first
//    time, a job of its own: GpuReady) and renders; Skia submits its work to
//    the queue before jsui_render_gpu_target returns.
//
// This frame's Slate draw, which samples the render target, is recorded on
// the render thread after our command, so it reaches the GPU after Skia's
// work. How each RHI orders it and keeps the texture's state in step with
// what Unreal tracks is described in its file.
//
// The context is shared by every view: only jobs touch it (they run one at a
// time, all in the same place), the game thread only checks whether it
// exists. It lives until the engine's pre-exit (ReleaseGpu, from
// JsuiModule.cpp), so it never outlives the RHI's device.
namespace
{
std::atomic<JsuiGpuContext*> GGpu{nullptr};
std::atomic<bool> GGpuUnavailable{false};  // creation failed (or the context was released)
bool GGpuCreateQueued = false;             // game thread
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
		UE_LOG(LogJsui, Warning, TEXT("a render job did not run within 2 s; going on without it (CPU renderer)"));
		Done = Queued;
		// The RHI is not running our jobs: stop queueing them.
		bGpuFailed = true;
		if (!GGpu.load()) GGpuUnavailable = true;
	}
}

void FJsuiHost::Shutdown()
{
	WaitRender();
	if (Rt) jsui_destroy(Rt);
	Rt = nullptr;
	if (GpuApi() == EGpuApi::Vulkan)
	{
		// Render-thread state: let it go there, while the RHI is up.
		TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
		ENQUEUE_RENDER_COMMAND(JsuiReleaseTarget)([Self](FRHICommandListImmediate&) { Self->VulkanTarget.SafeRelease(); });
	}
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

FJsuiHost::EGpuApi FJsuiHost::GpuApi()
{
	if (!GDynamicRHI) return EGpuApi::None;
	switch (GDynamicRHI->GetInterfaceType())
	{
#if JSUI_UE_METAL
	case ERHIInterfaceType::Metal: return EGpuApi::Metal;
#endif
#if JSUI_UE_VULKAN
	case ERHIInterfaceType::Vulkan: return EGpuApi::Vulkan;
#endif
#if JSUI_UE_D3D12
	case ERHIInterfaceType::D3D12: return EGpuApi::D3D12;
#endif
#if JSUI_UE_D3D11
	case ERHIInterfaceType::D3D11: return EGpuApi::D3D11;
#endif
	default: return EGpuApi::None;
	}
}

bool FJsuiHost::GpuSupported()
{
	return GpuApi() != EGpuApi::None && !GGpuUnavailable;
}

const TCHAR* FJsuiHost::GpuRendererName()
{
	const EGpuApi Api = GpuApi();
	// Skia Graphite has no Direct3D backend: Ganesh on Direct3D 12.
	return Api == EGpuApi::D3D12 || Api == EGpuApi::D3D11 ? TEXT("Skia Ganesh (Direct3D 12)") : TEXT("Skia Graphite");
}

bool FJsuiHost::GpuReady()
{
	if (GGpu.load()) return true;
	if (GGpuUnavailable || GGpuCreateQueued) return false;
	GGpuCreateQueued = true;
	QueueJob();
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	const EGpuApi Api = GpuApi();
	ENQUEUE_RENDER_COMMAND(JsuiCreateGpu)(
		[Self, Api](FRHICommandListImmediate& RHICmdList)
		{
			switch (Api)
			{
#if JSUI_UE_METAL
			case EGpuApi::Metal: Self->InitMetal(RHICmdList); break;
#endif
#if JSUI_UE_VULKAN
			case EGpuApi::Vulkan: Self->InitVulkan(RHICmdList); break;
#endif
#if JSUI_UE_D3D12
			case EGpuApi::D3D12: Self->InitD3D12(RHICmdList); break;
#endif
#if JSUI_UE_D3D11
			case EGpuApi::D3D11: Self->InitD3D11(RHICmdList); break;
#endif
			default:
				GGpuUnavailable = true;
				Self->FinishJob();
				break;
			}
		});
	return false;
}

bool FJsuiHost::GpuFailed()
{
	WaitRender();
	return bGpuFailed || GGpuUnavailable;
}

void FJsuiHost::QueueGpuRender(FTextureRenderTargetResource* Target, int32 Width, int32 Height, bool bRepaint)
{
	const EGpuApi Api = GpuApi();
	if (Api == EGpuApi::None) return;
	QueueJob();
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Self = AsShared();
	ENQUEUE_RENDER_COMMAND(JsuiRender)(
		[Self, Api, Target, Width, Height, bRepaint](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Texture = Target ? Target->TextureRHI.GetReference() : nullptr;
			if (!Texture || Self->bGpuFailed || !GGpu.load())
			{
				Self->FinishJob();
				return;
			}
			switch (Api)
			{
#if JSUI_UE_METAL
			case EGpuApi::Metal: Self->RenderMetal(RHICmdList, Texture, Width, Height, bRepaint); break;
#endif
#if JSUI_UE_VULKAN
			case EGpuApi::Vulkan: Self->RenderVulkan(RHICmdList, Texture, Width, Height, bRepaint); break;
#endif
#if JSUI_UE_D3D12
			case EGpuApi::D3D12: Self->RenderD3D12(RHICmdList, Texture, Width, Height, bRepaint); break;
#endif
#if JSUI_UE_D3D11
			case EGpuApi::D3D11: Self->RenderD3D11(RHICmdList, Texture, Width, Height, bRepaint); break;
#endif
			default: Self->FinishJob(); break;
			}
		});
}

void FJsuiHost::CreateJob(TFunctionRef<JsuiGpuContext*()> Create)
{
	if (!GGpu.load() && !GGpuUnavailable)
	{
		if (JsuiGpuContext* Gpu = Create())
		{
			GGpu = Gpu;
			UE_LOG(LogJsui, Log, TEXT("GPU: %s on the %s RHI's device"), GpuRendererName(), GDynamicRHI->GetName());
		}
		else
		{
			GGpuUnavailable = true;
			UE_LOG(LogJsui, Warning, TEXT("GPU: no %s context on the %s RHI's device (the library was built without it, or Skia rejected the device)"),
			       GpuRendererName(), GDynamicRHI->GetName());
		}
	}
	FinishJob();
}

void FJsuiHost::RenderJob(JsuiGpuTarget Target, bool bRepaint)
{
	JsuiGpuContext* Gpu = GGpu.load();
	if (Rt && !bGpuFailed && Target.texture)
	{
		Target.flags = (!bRepaint && HeldTexture == Target.texture) ? JSUI_GPU_INCREMENTAL : 0;
		JsuiRect Dirty;
		if (Gpu && jsui_render_gpu_target(Rt, Gpu, &Target, &Dirty) == 0)
		{
			HeldTexture = Target.texture;
		}
		else
		{
			HeldTexture = nullptr;
			bGpuFailed = true;
		}
	}
	FinishJob();
}

void FJsuiHost::QueueJob()
{
	std::lock_guard<std::mutex> Lock(JobMutex);
	Queued++;
}

void FJsuiHost::FinishJob()
{
	std::lock_guard<std::mutex> Lock(JobMutex);
	Done++;
	JobDone.notify_all();
}

void FJsuiHost::ReleaseGpu()
{
	const EGpuApi Api = GpuApi();
	// Metal: the context holds references to the RHI's device and queue and
	// is freed with the process.
	if (!GGpu.load() || Api == EGpuApi::None || Api == EGpuApi::Metal) return;
	GGpuUnavailable = true;  // views that still render fall back to the CPU
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Released = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	ENQUEUE_RENDER_COMMAND(JsuiReleaseGpu)(
		[Api, Released](FRHICommandListImmediate& RHICmdList)
		{
			TFunction<void()> Job = [Released]
			{
				if (JsuiGpuContext* Gpu = GGpu.exchange(nullptr)) jsui_gpu_destroy(Gpu);
				*Released = true;
			};
			switch (Api)
			{
#if JSUI_UE_VULKAN
			case EGpuApi::Vulkan: ReleaseVulkan(RHICmdList, MoveTemp(Job)); break;
#endif
#if JSUI_UE_D3D12
			case EGpuApi::D3D12: ReleaseD3D12(RHICmdList, MoveTemp(Job)); break;
#endif
#if JSUI_UE_D3D11
			case EGpuApi::D3D11: ReleaseD3D11(RHICmdList, MoveTemp(Job)); break;
#endif
			default: break;
			}
		});
	FlushRenderingCommands();
	// The job runs on the RHI's submission side; give it a moment (an RHI that
	// stopped never runs it: the context then goes with the process).
	const double Until = FPlatformTime::Seconds() + 2.0;
	while (!*Released && FPlatformTime::Seconds() < Until) FPlatformProcess::Sleep(0.001f);
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
