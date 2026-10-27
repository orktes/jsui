#include "JsuiHost.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsuiLog.h"
#include "Misc/EngineVersionComparison.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "TextureResource.h"

#include <string>
#include <vector>

#if JSUI_UE_METAL
#include "IMetalDynamicRHI.h"  // IsRHIMetal
#endif

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

// ------------------------------------------------------------ Engine HTTP
//
// JsuiConfig.on_request answers http(s):// requests through FHttpModule (the
// platform's HTTP stack: NSURLSession on Apple, libcurl or WinHTTP elsewhere,
// with the engine's proxy and certificate settings). Everything else (files,
// data:) is declined and goes to the built-in loader, and so are requests
// with redirect: 'error' or 'manual': FHttpModule always follows redirects.
//
// on_request runs where the runtime starts loads (the game thread, or JS's
// own thread): the request is started on the game thread. Its delegates run
// on the HTTP thread (CompleteOnHttpThread) and answer the runtime from there
// (jsui_request_* are thread safe): the status and headers once the first
// body bytes (or the end) arrive, then each piece of the body as it comes
// (streamed requests) or the whole body at the end. Shutdown() cancels what
// is in flight and no answer reaches the runtime after that.
#define JSUI_UE_HTTP_STREAMING UE_VERSION_NEWER_THAN(5, 4, 99)

struct FJsuiHttp : public TSharedFromThis<FJsuiHttp, ESPMode::ThreadSafe>
{
	struct FCall
	{
		FHttpRequestPtr Request;
		bool bStreaming = false;
		bool bResponded = false;  // jsui_request_respond ran
	};
	struct FStart
	{
		FString Url, Verb;
		TArray<TPair<FString, FString>> Headers;
		TArray<uint8> Body;
		int32 TimeoutMs = 0;
		bool bStreaming = false;
	};

	std::mutex Mutex;
	JsuiRuntime* Rt = nullptr;  // null once the runtime goes away
	TMap<uint64, TSharedPtr<FCall, ESPMode::ThreadSafe>> Calls;
	TSet<uint64> Starting;   // taken, not started on the game thread yet
	TSet<uint64> Cancelled;  // cancelled while starting

	static int OnRequest(void* User, JsuiRuntime*, uint64_t Id, const JsuiRequest* Req)
	{
		FJsuiHttp* Self = static_cast<FJsuiHttp*>(User);
		const FString Url = UTF8_TO_TCHAR(Req->url ? Req->url : "");
		if (!Url.StartsWith(TEXT("http://")) && !Url.StartsWith(TEXT("https://"))) return 1;
		if (Req->redirect != JSUI_REDIRECT_FOLLOW) return 1;
		FStart S;
		S.Url = Url;
		S.Verb = UTF8_TO_TCHAR(Req->method ? Req->method : "GET");
		for (size_t i = 0; i < Req->header_count; i++)
		{
			const JsuiHeader& H = Req->headers[i];
			if (!H.name) continue;
			const FString Name = UTF8_TO_TCHAR(H.name);
			// Framing headers are the HTTP stack's to set.
			if (Name == TEXT("host") || Name == TEXT("content-length") || Name == TEXT("connection") ||
			    Name == TEXT("transfer-encoding") || Name == TEXT("accept-encoding"))
				continue;
			S.Headers.Emplace(Name, UTF8_TO_TCHAR(H.value ? H.value : ""));
		}
		if (Req->body && Req->body_size) S.Body.Append(static_cast<const uint8*>(Req->body), int32(Req->body_size));
		S.TimeoutMs = Req->timeout_ms;
#if JSUI_UE_HTTP_STREAMING
		S.bStreaming = Req->streaming != 0;
#endif
		{
			std::lock_guard<std::mutex> Lock(Self->Mutex);
			if (!Self->Rt) return 1;
			Self->Starting.Add(Id);
		}
		if (IsInGameThread())
		{
			Self->Start(Id, MoveTemp(S));
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread,
			          [Shared = Self->AsShared(), Id, S = MoveTemp(S)]() mutable { Shared->Start(Id, MoveTemp(S)); });
		}
		return 0;
	}

	static void OnCancel(void* User, JsuiRuntime*, uint64_t Id)
	{
		FJsuiHttp* Self = static_cast<FJsuiHttp*>(User);
		FHttpRequestPtr Request;
		{
			std::lock_guard<std::mutex> Lock(Self->Mutex);
			if (Self->Starting.Contains(Id))
			{
				Self->Cancelled.Add(Id);
				return;
			}
			TSharedPtr<FCall, ESPMode::ThreadSafe> Call;
			if (!Self->Calls.RemoveAndCopyValue(Id, Call)) return;
			Request = Call->Request;
		}
		if (IsInGameThread()) Request->CancelRequest();
		else AsyncTask(ENamedThreads::GameThread, [Request] { Request->CancelRequest(); });
	}

	// Game thread.
	void Start(uint64 Id, FStart S)
	{
		FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Starting.Remove(Id);
			if (Cancelled.Remove(Id) || !Rt) return;
			Request->SetURL(S.Url);
			Request->SetVerb(S.Verb);
			for (const TPair<FString, FString>& H : S.Headers) Request->SetHeader(H.Key, H.Value);
			if (S.Body.Num()) Request->SetContent(MoveTemp(S.Body));
			if (S.TimeoutMs > 0) Request->SetTimeout(float(S.TimeoutMs) / 1000.f);
			TSharedPtr<FCall, ESPMode::ThreadSafe> Call = MakeShared<FCall, ESPMode::ThreadSafe>();
			Call->Request = Request;
			Call->bStreaming = S.bStreaming;
			TWeakPtr<FJsuiHttp, ESPMode::ThreadSafe> Weak = AsShared();
#if JSUI_UE_HTTP_STREAMING
			Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
			if (S.bStreaming)
			{
				// HTTP thread. Setting Length to 0 fails the request (the
				// runtime is gone, or no longer wants it).
				Request->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(
					[Weak, Id](void* Ptr, int64& Length)
					{
						TSharedPtr<FJsuiHttp, ESPMode::ThreadSafe> Self = Weak.Pin();
						if (!Self || !Self->Body(Id, Ptr, Length)) Length = 0;
					}));
			}
#endif
			Request->OnProcessRequestComplete().BindLambda(
				[Weak, Id](FHttpRequestPtr Req, FHttpResponsePtr Response, bool bSucceeded)
				{
					if (TSharedPtr<FJsuiHttp, ESPMode::ThreadSafe> Self = Weak.Pin()) Self->Complete(Id, Req, Response, bSucceeded);
				});
			Calls.Add(Id, Call);
		}
		Request->ProcessRequest();
	}

	// The status, headers and final URL (once; Mutex held).
	void RespondLocked(uint64 Id, FCall& Call, const FHttpResponsePtr& Response)
	{
		Call.bResponded = true;
		bool bDecoded = false;
		const TArray<FString> All = Response->GetAllHeaders();
		for (const FString& Line : All)
		{
			FString Name, Value;
			if (!Line.Split(TEXT(":"), &Name, &Value)) continue;
			Name.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			bDecoded |= Name.Equals(TEXT("content-encoding"), ESearchCase::IgnoreCase) && !Value.Equals(TEXT("identity"), ESearchCase::IgnoreCase);
		}
		std::vector<std::string> Strings;
		Strings.reserve(All.Num() * 2);
		for (const FString& Line : All)
		{
			FString Name, Value;
			if (!Line.Split(TEXT(":"), &Name, &Value)) continue;
			Name.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			// The stack decoded a compressed body: its encoding and length
			// describe bytes JS never sees.
			if (bDecoded && (Name.Equals(TEXT("content-encoding"), ESearchCase::IgnoreCase) ||
			                 Name.Equals(TEXT("content-length"), ESearchCase::IgnoreCase)))
				continue;
			Strings.emplace_back(TCHAR_TO_UTF8(*Name));
			Strings.emplace_back(TCHAR_TO_UTF8(*Value));
		}
		std::vector<JsuiHeader> Headers;
		for (size_t i = 0; i + 1 < Strings.size(); i += 2) Headers.push_back({Strings[i].c_str(), Strings[i + 1].c_str()});
#if JSUI_UE_HTTP_STREAMING
		const FString FinalUrl = Call.Request->GetEffectiveURL();
#else
		const FString FinalUrl = Response->GetURL();
#endif
		const std::string Url(TCHAR_TO_UTF8(*FinalUrl));
		jsui_request_respond(Rt, Id, Response->GetResponseCode(), nullptr, Url.empty() ? nullptr : Url.c_str(),
		                     Headers.data(), Headers.size());
	}

	// HTTP thread: a piece of a streamed body.
	bool Body(uint64 Id, const void* Ptr, int64 Length)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		TSharedPtr<FCall, ESPMode::ThreadSafe>* Call = Calls.Find(Id);
		if (!Call || !Rt) return false;
		if (!(*Call)->bResponded)
		{
			FHttpResponsePtr Response = (*Call)->Request->GetResponse();
			if (!Response) return false;
			RespondLocked(Id, **Call, Response);
		}
		if (Length > 0) jsui_request_body(Rt, Id, Ptr, size_t(Length));
		return true;
	}

	void Complete(uint64 Id, FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		TSharedPtr<FCall, ESPMode::ThreadSafe> Call;
		if (!Calls.RemoveAndCopyValue(Id, Call) || !Rt) return;
		if (!bSucceeded || !Response || Response->GetResponseCode() <= 0)
		{
			FString Why;
#if JSUI_UE_HTTP_STREAMING
			const EHttpFailureReason Reason = Request ? Request->GetFailureReason() : EHttpFailureReason::Other;
			Why = Reason == EHttpFailureReason::TimedOut ? FString(TEXT("timed out"))
			                                             : FString::Printf(TEXT("network error (%s)"), LexToString(Reason));
#else
			Why = FString::Printf(TEXT("network error (%s)"),
			                      Request ? EHttpRequestStatus::ToString(Request->GetStatus()) : TEXT("no request"));
#endif
			const std::string Error(TCHAR_TO_UTF8(*Why));
			if (Call->bResponded) jsui_request_end(Rt, Id, Error.c_str());
			else jsui_request_respond(Rt, Id, 0, Error.c_str(), nullptr, nullptr, 0);
			return;
		}
		if (!Call->bResponded) RespondLocked(Id, *Call, Response);
		if (!Call->bStreaming)
		{
			const TArray<uint8>& Content = Response->GetContent();
			if (Content.Num()) jsui_request_body(Rt, Id, Content.GetData(), size_t(Content.Num()));
		}
		jsui_request_end(Rt, Id, nullptr);
	}

	// Game thread, before the runtime is destroyed: nothing answers it after this.
	void Close()
	{
		TArray<FHttpRequestPtr> InFlight;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Rt = nullptr;
			for (const auto& It : Calls) InFlight.Add(It.Value->Request);
			Calls.Reset();
			Starting.Reset();
			Cancelled.Reset();
		}
		for (const FHttpRequestPtr& Request : InFlight) Request->CancelRequest();
	}
};

TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> FJsuiHost::Create(int32 Width, int32 Height, float PixelRatio, bool bDevMode,
                                                         int32 JsThread, bool bUseEngineHttp)
{
	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Host(new FJsuiHost());
	JsuiConfig Cfg;
	jsui_config_init(&Cfg);
	Cfg.width = FMath::Max(1, Width);
	Cfg.height = FMath::Max(1, Height);
	Cfg.pixel_ratio = PixelRatio;
	Cfg.dev_mode = bDevMode ? 1 : 0;
	Cfg.js_thread = JsThread;
	// Called inside jsui_tick / jsui_sync / jsui_send_* on the game thread
	// (also with JS on its own thread: its callbacks are delivered in the
	// tick; maybe a log line from a render job): queued, drained by SJsuiView
	// after the tick.
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
	if (bUseEngineHttp)
	{
		Host->Http = MakeShared<FJsuiHttp, ESPMode::ThreadSafe>();
		Cfg.request_user = Host->Http.Get();
		Cfg.on_request = &FJsuiHttp::OnRequest;
		Cfg.on_request_cancel = &FJsuiHttp::OnCancel;
	}
	Host->Rt = jsui_create(&Cfg);
	if (!Host->Rt) return nullptr;
	if (Host->Http)
	{
		std::lock_guard<std::mutex> Lock(Host->Http->Mutex);
		Host->Http->Rt = Host->Rt;
	}
	return Host;
}

FJsuiHost::~FJsuiHost()
{
	// Shutdown() normally ran on the game thread already.
	if (Http) Http->Close();
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
	if (Http) Http->Close();
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
#if JSUI_UE_METAL
	// The Metal RHI's own test (as before this file had other RHIs).
	if (IsRHIMetal()) return EGpuApi::Metal;
#endif
	switch (GDynamicRHI->GetInterfaceType())
	{
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
