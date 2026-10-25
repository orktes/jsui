// SJsuiView: a JSUI runtime drawn over the game as a Slate widget.
//
// Put it over the game viewport (UJsuiWidget does that from UMG). It loads the
// bundle on its first tick, forwards mouse, keyboard and text input, ticks the
// runtime once per Slate tick and draws the UI texture with premultiplied
// alpha.
//
// Rendering. On the GPU, by default: Skia draws the UI into a
// UTextureRenderTarget2D on the RHI's own device and queue, repainting only
// what changed (JsuiHost.cpp): Graphite on Metal (macOS) and Vulkan (Linux,
// Windows with -vulkan), Ganesh on D3D12 and D3D11 (Windows). On any other
// RHI, with bForceCpu, or after the GPU path failed, the runtime rasterises
// on the CPU (jsui_render_rgba_incremental) and the changed rectangle is
// uploaded into a UTexture2D. Either texture is
// premultiplied RGBA holding sRGB-encoded values in a non-sRGB format, drawn
// with ESlateDrawEffect::PreMultipliedAlpha | NoGamma.
//
// Input pass-through. An input preprocessor sees every mouse and key event
// before Slate routes it; jsui_send_* says whether the UI consumed it.
// Consumed events stop there; the rest go on to the game as usual and are
// also raised as the OnPassed* delegates, so a game can react only to input
// that was not meant for the UI. Typed text arrives through OnKeyChar while
// the view has keyboard focus (it takes it when the UI consumes a press).
//
// Game <-> UI. Messages from JS (Host.postMessage) arrive as OnMessage after
// each tick; SetState / PostMessage / SetBuffer feed the UI (useHostState,
// Host.addListener, useHostBufferValue).
//
// JS thread. By default the bundle runs on a thread of its own, as in React
// Native (JsThread = JSUI_JS_THREAD_ON): the Slate tick never waits for JS,
// and what native does every frame (Animated values driven by SetBuffer
// data, scrolling, press styles) goes on while JS is busy. What JS does
// lands a frame or more later; SyncJsNextFrame() / SyncJs() when a change
// must show on this frame. OnMessage still fires on the game thread, after
// the tick (or SyncJs).
#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Styling/SlateBrush.h"
#include "UObject/GCObject.h"
#include "Widgets/SLeafWidget.h"

struct JsuiRuntime;
class FJsuiHost;
class FJsuiInputProcessor;
class UTexture2D;
class UTextureRenderTarget2D;

class JSUI_API SJsuiView : public SLeafWidget, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SJsuiView)
		: _Bundle(TEXT("Jsui/bundle.js")), _bDevMode(false), _bForceCpu(false), _bForwardInput(true), _PixelRatio(0.f), _JsThread(1)
	{}
		/** Bundle path relative to the project's Content dir, an absolute path, or an http:// URL (jsui dev). */
		SLATE_ARGUMENT(FString, Bundle)
		/** Poll the dev server and hot-update (with an http:// bundle from `npx jsui dev`). */
		SLATE_ARGUMENT(bool, bDevMode)
		/** Rasterise on the CPU even where the GPU path is available. */
		SLATE_ARGUMENT(bool, bForceCpu)
		/** Forward mouse and keyboard input to the UI. */
		SLATE_ARGUMENT(bool, bForwardInput)
		/** Device pixels per logical px; 0: the screen's DPI scale (2 on Retina). */
		SLATE_ARGUMENT(float, PixelRatio)
		/** JSUI_JS_THREAD_OFF (0), _ON (1, the default) or _LOCKSTEP (2). */
		SLATE_ARGUMENT(int32, JsThread)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SJsuiView();

	// ------------------------------------------------------------ API

	/** The runtime, for direct jsui_* calls (waits for a queued GPU render first). */
	JsuiRuntime* Runtime() const;
	void SetState(const FString& Key, const FString& Json) const;
	void PostMessage(const FString& Name, const FString& Json) const;
	/** Writes `Count` floats into the frame buffer `Key` (Float32Array in JS) and commits it. */
	void SetBuffer(const FString& Key, const float* Values, int32 Count) const;
	/** jsui_sync: waits up to TimeoutMs (negative: no limit) for JS to handle what was sent to it and applies it;
	 *  broadcasts the messages JS posted meanwhile. True when JS is left idle. */
	bool SyncJs(double TimeoutMs = 250.0);
	/** The next tick is a JSUI_TICK_SYNC_JS one: what JS does for it lands on that frame. */
	void SyncJsNextFrame() { bSyncJsNextFrame = true; }
	/** jsui_debug_stall_js: keeps JS busy for Ms (demos, tests). */
	void DebugStallJs(double Ms) const;
	/** The JSUI_JS_THREAD_* mode the runtime runs in. */
	int32 GetJsThread() const;

	/** Viewport size in logical px (valid after the first tick). */
	FVector2D GetUiSize() const { return FVector2D(Width, Height); }
	float GetPixelRatio() const { return Ratio; }
	/** How the UI is drawn, e.g. for a stats overlay. */
	FString GetRendererName() const;
	/** Whether the last left press went to the UI (a drag that started there is not the game's). */
	bool IsPointerCaptured() const { return bPointerCaptured; }
	/** Desktop position (a pointer event's screen-space position, FSlateApplication::GetCursorPos)
	 *  to UI coordinates (logical px, top-left of the view). */
	FVector2D AbsoluteToUi(const FVector2D& Screen) const;
	void SetForwardInput(bool bForward) { bForwardInput = bForward; }

	/** Host.postMessage(name, payload) from JS: name and the payload as JSON. */
	TMulticastDelegate<void(const FString& /*Name*/, const FString& /*Json*/)> OnMessage;
	/** A mouse press the UI did not consume: the button and its position (logical px). */
	TMulticastDelegate<void(const FKey& /*Button*/, const FVector2D& /*Position*/)> OnPassedMouseDown;
	/** The release of a press the UI did not consume (OnPassedMouseDown): the button and its position (logical px). */
	TMulticastDelegate<void(const FKey& /*Button*/, const FVector2D& /*Position*/)> OnPassedMouseUp;
	/** A wheel movement the UI did not consume (notches, positive away from the user). */
	TMulticastDelegate<void(float /*Delta*/)> OnPassedWheel;
	/** A key press (not a repeat) the UI did not consume. */
	TMulticastDelegate<void(const FKey& /*Key*/)> OnPassedKeyDown;

	// ------------------------------------------------------------ SWidget

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	                      FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1, 1); }
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;

	// ------------------------------------------------------------ FGCObject

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SJsuiView"); }

private:
	friend class FJsuiInputProcessor;

	// Called by the input preprocessor (game thread) before Slate routes the
	// event. Return whether the UI consumed it.
	bool HandleMouseMove(const FPointerEvent& Event);
	bool HandleMouseButton(const FPointerEvent& Event, bool bDown);
	bool HandleWheel(const FPointerEvent& Event);
	bool HandleKey(const FKeyEvent& Event, bool bDown);

	void Load();
	void UpdateViewport(const FGeometry& Geometry);
	void DrainQueues();
	void Paint();
	void PaintGpu(int32 PixelWidth, int32 PixelHeight);
	void PaintCpu(int32 PixelWidth, int32 PixelHeight);
	bool InsideView(const FVector2D& Screen) const;
	void UpdateWindowOffset();
	void OnActivationChanged(bool bActive);
	void ReleaseKeys();

	TSharedPtr<FJsuiHost, ESPMode::ThreadSafe> Host;
	TSharedPtr<FJsuiInputProcessor> InputProcessor;
	FString Bundle;
	bool bDevMode = false, bForceCpu = false, bForwardInput = true;
	float RequestedRatio = 0.f;
	bool bLoaded = false;
	bool bSyncJsNextFrame = false;

	FGeometry LastGeometry;
	bool bHasGeometry = false;
	TWeakPtr<SWindow> Window;
	// Where Slate believes the view's window is minus where it is on the
	// desktop (nonzero for a fullscreen window on macOS: UpdateWindowOffset).
	FVector2D WindowOffset = FVector2D::ZeroVector;
	int32 Width = 1, Height = 1;  // logical px
	float Ratio = 1.f;

	bool bGpu = false;
	TObjectPtr<UTextureRenderTarget2D> Target = nullptr;
	TObjectPtr<UTexture2D> CpuTexture = nullptr;
	bool bRepaint = true;
	mutable FSlateBrush Brush;

	FVector2D Mouse = FVector2D(-1, -1);
	bool bMouseInside = false;
	TSet<FKey> ButtonsDown;
	TSet<FKey> ButtonsConsumed;  // presses the UI took: their releases are swallowed too
	TSet<FKey> KeysDown;
	TSet<FKey> KeysConsumed;
	bool bPointerCaptured = false;
};
