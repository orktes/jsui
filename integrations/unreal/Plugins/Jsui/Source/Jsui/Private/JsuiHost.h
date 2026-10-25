// FJsuiHost: one JSUI runtime and what its renders share across threads.
//
// Threads. The game thread owns the runtime: SJsuiView ticks it, feeds it
// input, state and buffers. On the GPU path a render job paints it where the
// RHI submits its GPU work (Metal / Vulkan / D3D12: the RHI's submission side
// through RHIRunOnQueue; D3D11: the thread that owns the immediate context;
// see JsuiHost.cpp and JsuiGpu*.cpp), so the game thread must not touch the
// runtime until that job has run: Runtime() waits for every queued job
// first. Usually the job has run long before the next frame's first runtime
// call and the wait is free.
#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "Templates/Function.h"

THIRD_PARTY_INCLUDES_START
#include "jsui/jsui.h"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <condition_variable>
#include <mutex>

class FRHICommandListImmediate;
class FTextureRenderTargetResource;

class FJsuiHost : public TSharedFromThis<FJsuiHost, ESPMode::ThreadSafe>
{
public:
	struct FMessage
	{
		FString Name, Json;
	};
	struct FLog
	{
		int32 Level;
		FString Text;
	};

	// A runtime of Width x Height logical px at PixelRatio device px per px,
	// its JS in JsThread mode (JSUI_JS_THREAD_*).
	static TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Create(int32 Width, int32 Height, float PixelRatio, bool bDevMode,
	                                                         int32 JsThread);
	~FJsuiHost();

	// The runtime for a game-thread call (waits for a queued render job).
	JsuiRuntime* Runtime();
	// Waits until every queued render job has run (gives up after 2 s: the
	// RHI stopped, e.g. at shutdown).
	void WaitRender();
	// Waits for the render jobs and destroys the runtime (game thread).
	void Shutdown();

	// Host.postMessage calls and log lines queued since the last call.
	void TakeMessages(TArray<FMessage>& Out);
	void TakeLogs(TArray<FLog>& Out);

	// ---------------------------------------------------------- GPU path

	// The RHIs with a GPU path: Metal (macOS), Vulkan (Linux, Windows with
	// -vulkan), D3D12 and D3D11 (Windows).
	enum class EGpuApi : uint8
	{
		None,
		Metal,
		Vulkan,
		D3D12,
		D3D11
	};
	// The running RHI if the plugin has a GPU path for it (built for this
	// platform), else None.
	static EGpuApi GpuApi();
	// Whether the GPU path can be used: GpuApi() is not None and creating the
	// GPU context has not failed.
	static bool GpuSupported();
	// What draws the UI on the GPU path, e.g. "Skia Graphite".
	static const TCHAR* GpuRendererName();
	// Whether the GPU context exists. If not, queues its creation (once, as a
	// job of this host) and returns false: paint on a later tick. Game thread.
	bool GpuReady();
	// Whether a render job found the GPU path unusable (no Skia context on
	// this device, a failed render): switch to the CPU path.
	bool GpuFailed();
	// Frees the shared GPU context before the RHI goes away (engine pre-exit;
	// game thread). Later renders fail and views fall back to the CPU.
	static void ReleaseGpu();
	// Queues a render of the runtime into `Target` (a render target resource
	// of Width x Height device px). bRepaint: the texture does not hold the
	// last frame (new, or its contents were lost): paint it in full.
	void QueueGpuRender(FTextureRenderTargetResource* Target, int32 Width, int32 Height, bool bRepaint);
	// Whether a job skipped its render and wants another one (clears it).
	bool TakeGpuRepaint() { return bGpuWantsRepaint.exchange(false); }

	// ---------------------------------------------------------- CPU path

	// Paints into the host's RGBA8 premultiplied buffer of Width x Height
	// device px, only what changed since the last call unless bForce.
	// Returns true and the repainted rectangle when pixels changed.
	bool RenderCpu(int32 Width, int32 Height, bool bForce, JsuiRect& Dirty);
	const uint8* Pixels() const { return CpuPixels.GetData(); }

private:
	FJsuiHost() = default;

	// Job bodies, run where the RHI submits GPU work; each ends the job.
	// Creates the shared context with Create() unless it exists or failed.
	void CreateJob(TFunctionRef<JsuiGpuContext*()> Create);
	// Paints the runtime into Target (flags are set here: incremental when
	// Target.texture still holds this runtime's last frame and !bRepaint).
	void RenderJob(JsuiGpuTarget Target, bool bRepaint);
	void FinishJob();
	void QueueJob();  // counts a job that will end with FinishJob()

	// Per RHI (JsuiGpuMetal.cpp, JsuiGpuVulkan.cpp, JsuiGpuD3D12.cpp,
	// JsuiGpuD3D11.cpp), on the render thread: queue a CreateJob / a
	// RenderJob into Texture (Width x Height device px) ordered with the
	// RHI's own work, leaving Texture in the state Unreal tracks for it.
	void InitMetal(FRHICommandListImmediate& RHICmdList);
	void RenderMetal(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint);
	void InitVulkan(FRHICommandListImmediate& RHICmdList);
	void RenderVulkan(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint);
	void InitD3D12(FRHICommandListImmediate& RHICmdList);
	void RenderD3D12(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint);
	void InitD3D11(FRHICommandListImmediate& RHICmdList);
	void RenderD3D11(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, int32 Width, int32 Height, bool bRepaint);
	// Per RHI, render thread: runs Job (which frees the shared context) where
	// the RHI submits its work, after everything recorded so far.
	static void ReleaseVulkan(FRHICommandListImmediate& RHICmdList, TFunction<void()>&& Job);
	static void ReleaseD3D12(FRHICommandListImmediate& RHICmdList, TFunction<void()>&& Job);
	static void ReleaseD3D11(FRHICommandListImmediate& RHICmdList, TFunction<void()>&& Job);

	JsuiRuntime* Rt = nullptr;

	std::mutex QueueMutex;  // messages / logs: the render job may log
	TArray<FMessage> Messages;
	TArray<FLog> Logs;

	// Render job fence.
	std::mutex JobMutex;
	std::condition_variable JobDone;
	uint64 Queued = 0, Done = 0;

	// Render job state (only the job touches these).
	void* HeldTexture = nullptr;  // the texture holding this runtime's last frame
	std::atomic<bool> bGpuFailed{false};

	// Vulkan (render thread only): the image Graphite draws into, copied into
	// the view's render target (Graphite needs INPUT_ATTACHMENT usage, which
	// a UTextureRenderTarget2D's image does not have).
	FTextureRHIRef VulkanTarget;
	// Set (by a background task) once VulkanTarget's creation is certainly on
	// the GPU queue (JsuiGpuVulkan.cpp); renders into it wait for that.
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> VulkanTargetSubmitted;
	bool bVulkanTargetFresh = false;  // not rendered into yet
	// A render job skipped its render (the target was not ready): the view
	// queues another one even if the UI did not change.
	std::atomic<bool> bGpuWantsRepaint{false};

	// CPU path.
	TArray<uint8> CpuPixels;
	int32 CpuWidth = 0, CpuHeight = 0;
	bool bCpuFresh = true;
};
