// FJsuiHost: one JSUI runtime and what its renders share across threads.
//
// Threads. The game thread owns the runtime: SJsuiView ticks it, feeds it
// input, state and buffers. On the GPU path a render job paints it on the
// Metal RHI's submission side (IMetalDynamicRHI::RHIRunOnQueue, see
// JsuiHost.cpp), so the game thread must not touch the runtime until that job
// has run: Runtime() waits for every queued job first. Usually the job has
// run long before the next frame's first runtime call and the wait is free.
#pragma once

#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "jsui/jsui.h"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <condition_variable>
#include <mutex>

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

	// A runtime of Width x Height logical px at PixelRatio device px per px.
	static TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Create(int32 Width, int32 Height, float PixelRatio, bool bDevMode);
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

	// Whether the GPU path can be used: the plugin has it (macOS) and the RHI
	// is Metal.
	static bool GpuSupported();
	// Whether a render job found the GPU path unusable (no Skia context on
	// this device, a failed render): switch to the CPU path.
	bool GpuFailed();
	// Queues a render of the runtime into `Target` (a render target resource
	// of Width x Height device px). bRepaint: the texture does not hold the
	// last frame (new, or its contents were lost): paint it in full.
	void QueueGpuRender(FTextureRenderTargetResource* Target, int32 Width, int32 Height, bool bRepaint);

	// ---------------------------------------------------------- CPU path

	// Paints into the host's RGBA8 premultiplied buffer of Width x Height
	// device px, only what changed since the last call unless bForce.
	// Returns true and the repainted rectangle when pixels changed.
	bool RenderCpu(int32 Width, int32 Height, bool bForce, JsuiRect& Dirty);
	const uint8* Pixels() const { return CpuPixels.GetData(); }

private:
	FJsuiHost() = default;
	void RenderJob(void* Device, void* Queue, void* Texture, int32 Width, int32 Height, bool bRepaint);
	void FinishJob();

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

	// CPU path.
	TArray<uint8> CpuPixels;
	int32 CpuWidth = 0, CpuHeight = 0;
	bool bCpuFresh = true;
};
