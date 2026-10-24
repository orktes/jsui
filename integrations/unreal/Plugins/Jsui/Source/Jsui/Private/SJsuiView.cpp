#include "SJsuiView.h"

#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "JsuiHost.h"
#include "JsuiKeys.h"
#include "JsuiLog.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "Rendering/DrawElements.h"
#include "TextureResource.h"

// Every mouse and key event goes to the view before Slate routes it.
class FJsuiInputProcessor : public IInputProcessor
{
public:
	explicit FJsuiInputProcessor(SJsuiView* InView) : View(InView) {}

	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
	virtual bool HandleMouseMoveEvent(FSlateApplication&, const FPointerEvent& E) override { return View->HandleMouseMove(E); }
	virtual bool HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent& E) override { return View->HandleMouseButton(E, true); }
	// Slate sends the second press of a double click as this instead of a down.
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication&, const FPointerEvent& E) override { return View->HandleMouseButton(E, true); }
	virtual bool HandleMouseButtonUpEvent(FSlateApplication&, const FPointerEvent& E) override { return View->HandleMouseButton(E, false); }
	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& E, const FPointerEvent*) override { return View->HandleWheel(E); }
	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& E) override { return View->HandleKey(E, true); }
	virtual bool HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& E) override { return View->HandleKey(E, false); }
	virtual const TCHAR* GetDebugName() const override { return TEXT("JsuiInput"); }

private:
	SJsuiView* View;  // unregistered in ~SJsuiView
};

namespace
{
int Mods(const FModifierKeysState& M)
{
	int Out = 0;
	if (M.IsShiftDown()) Out |= JSUI_MOD_SHIFT;
	if (M.IsControlDown()) Out |= JSUI_MOD_CTRL;
	if (M.IsAltDown()) Out |= JSUI_MOD_ALT;
	if (M.IsCommandDown()) Out |= JSUI_MOD_META;
	return Out;
}

bool MouseButton(const FKey& Key, JsuiMouseButton& Out)
{
	if (Key == EKeys::LeftMouseButton) Out = JSUI_MOUSE_LEFT;
	else if (Key == EKeys::RightMouseButton) Out = JSUI_MOUSE_RIGHT;
	else if (Key == EKeys::MiddleMouseButton) Out = JSUI_MOUSE_MIDDLE;
	else return false;
	return true;
}
}  // namespace

void SJsuiView::Construct(const FArguments& InArgs)
{
	Bundle = InArgs._Bundle;
	bDevMode = InArgs._bDevMode;
	bForceCpu = InArgs._bForceCpu;
	bForwardInput = InArgs._bForwardInput;
	RequestedRatio = InArgs._PixelRatio;

	// The viewport size is known at the first tick (UpdateViewport).
	Host = FJsuiHost::Create(1, 1, 1.f, bDevMode);
	if (!Host)
	{
		UE_LOG(LogJsui, Error, TEXT("jsui_create failed"));
		return;
	}
	bGpu = !bForceCpu && FJsuiHost::GpuSupported();
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.Tiling = ESlateBrushTileType::NoTile;

	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FJsuiInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor, 0);
		FSlateApplication::Get().OnApplicationActivationStateChanged().AddSP(this, &SJsuiView::OnActivationChanged);
	}
}

SJsuiView::~SJsuiView()
{
	if (InputProcessor && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
		FSlateApplication::Get().OnApplicationActivationStateChanged().RemoveAll(this);
	}
	if (Host) Host->Shutdown();
}

// ------------------------------------------------------------ API

JsuiRuntime* SJsuiView::Runtime() const { return Host ? Host->Runtime() : nullptr; }

void SJsuiView::SetState(const FString& Key, const FString& Json) const
{
	if (JsuiRuntime* Rt = Runtime()) jsui_set_state(Rt, TCHAR_TO_UTF8(*Key), TCHAR_TO_UTF8(*Json));
}

void SJsuiView::PostMessage(const FString& Name, const FString& Json) const
{
	if (JsuiRuntime* Rt = Runtime()) jsui_post_message(Rt, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*Json));
}

void SJsuiView::SetBuffer(const FString& Key, const float* Values, int32 Count) const
{
	JsuiRuntime* Rt = Runtime();
	if (!Rt) return;
	FTCHARToUTF8 K(*Key);
	float* Out = static_cast<float*>(jsui_map_buffer(Rt, K.Get(), JSUI_BUFFER_F32, size_t(Count)));
	if (!Out) return;
	FMemory::Memcpy(Out, Values, sizeof(float) * Count);
	jsui_commit_buffer(Rt, K.Get());
}

FString SJsuiView::GetRendererName() const
{
	const TCHAR* Rhi = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("no RHI");
	return bGpu ? FString::Printf(TEXT("Skia Graphite · %s · RenderTarget2D"), Rhi)
	            : FString::Printf(TEXT("CPU raster · Texture2D · %s"), Rhi);
}

FVector2D SJsuiView::AbsoluteToUi(const FVector2D& Absolute) const
{
	if (!bHasGeometry) return FVector2D::ZeroVector;
	// Local Slate units -> device px -> logical px.
	return LastGeometry.AbsoluteToLocal(Absolute) * LastGeometry.Scale / Ratio;
}

bool SJsuiView::InsideView(const FVector2D& Absolute) const
{
	if (!bHasGeometry) return false;
	const FVector2D P = LastGeometry.AbsoluteToLocal(Absolute);
	const FVector2D S = LastGeometry.GetLocalSize();
	return P.X >= 0 && P.Y >= 0 && P.X < S.X && P.Y < S.Y;
}

// ---------------------------------------------------------- frame

void SJsuiView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	if (!Host) return;
	LastGeometry = AllottedGeometry;
	bHasGeometry = true;
	UpdateViewport(AllottedGeometry);
	if (!bLoaded) Load();
	jsui_tick(Runtime(), FPlatformTime::Seconds() * 1000.0);
	DrainQueues();
	Paint();
}

void SJsuiView::Load()
{
	bLoaded = true;
	FString Url = Bundle;
	if (!Bundle.Contains(TEXT("://")))
	{
		Url = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Bundle) ? FPaths::Combine(FPaths::ProjectContentDir(), Bundle) : Bundle);
	}
	JsuiRuntime* Rt = Runtime();
	if (jsui_load_bundle_url(Rt, TCHAR_TO_UTF8(*Url)) != 0)
	{
		UE_LOG(LogJsui, Error, TEXT("failed to load %s: %s"), *Url, UTF8_TO_TCHAR(jsui_last_error(Rt)));
	}
}

void SJsuiView::UpdateViewport(const FGeometry& Geometry)
{
	// Device px of the view: its local size times the geometry's scale
	// (Slate's absolute space is in pixels).
	const FVector2D Local = Geometry.GetLocalSize();
	const int32 PixelWidth = FMath::Max(1, FMath::RoundToInt(Local.X * Geometry.Scale));
	const int32 PixelHeight = FMath::Max(1, FMath::RoundToInt(Local.Y * Geometry.Scale));
	float R = RequestedRatio;
	if (R <= 0)
	{
		const FVector2D At = Geometry.GetAbsolutePosition();
		R = FMath::Max(1.f, FPlatformApplicationMisc::GetDPIScaleFactorAtPoint(At.X, At.Y));
	}
	const int32 W = FMath::Max(1, FMath::RoundToInt(PixelWidth / R));
	const int32 H = FMath::Max(1, FMath::RoundToInt(PixelHeight / R));
	if (W == Width && H == Height && R == Ratio && bLoaded) return;
	Width = W;
	Height = H;
	Ratio = R;
	jsui_set_viewport(Runtime(), W, H, R);
}

void SJsuiView::DrainQueues()
{
	TArray<FJsuiHost::FLog> Logs;
	Host->TakeLogs(Logs);
	for (const FJsuiHost::FLog& L : Logs)
	{
		if (L.Level >= JSUI_LOG_ERROR)
		{
			UE_LOG(LogJsui, Error, TEXT("%s"), *L.Text);
		}
		else if (L.Level == JSUI_LOG_WARN)
		{
			UE_LOG(LogJsui, Warning, TEXT("%s"), *L.Text);
		}
		else
		{
			UE_LOG(LogJsui, Log, TEXT("%s"), *L.Text);
		}
	}
	TArray<FJsuiHost::FMessage> Messages;
	Host->TakeMessages(Messages);
	for (const FJsuiHost::FMessage& M : Messages) OnMessage.Broadcast(M.Name, M.Json);
}

void SJsuiView::Paint()
{
	const int32 PixelWidth = FMath::Max(1, FMath::RoundToInt(Width * Ratio));
	const int32 PixelHeight = FMath::Max(1, FMath::RoundToInt(Height * Ratio));
	if (bGpu && Host->GpuFailed())
	{
		UE_LOG(LogJsui, Warning, TEXT("the GPU renderer failed; using the CPU renderer"));
		bGpu = false;
		Target = nullptr;
		bRepaint = true;
	}
	if (bGpu) PaintGpu(PixelWidth, PixelHeight);
	else PaintCpu(PixelWidth, PixelHeight);
}

void SJsuiView::PaintGpu(int32 PixelWidth, int32 PixelHeight)
{
	if (!Target || Target->SizeX != PixelWidth || Target->SizeY != PixelHeight)
	{
		if (!Target)
		{
			Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
			Target->ClearColor = FLinearColor::Transparent;
			Target->bAutoGenerateMips = false;
		}
		// BGRA8, not sRGB: the UI's sRGB-encoded values are stored as they
		// are and drawn with NoGamma.
		Target->InitCustomFormat(PixelWidth, PixelHeight, PF_B8G8R8A8, /*bInForceLinearGamma*/ true);
		Target->UpdateResourceImmediate(true);
		bRepaint = true;
	}
	if (!bRepaint && !jsui_needs_redraw(Runtime())) return;
	Host->QueueGpuRender(Target->GameThread_GetRenderTargetResource(), PixelWidth, PixelHeight, bRepaint);
	bRepaint = false;
}

void SJsuiView::PaintCpu(int32 PixelWidth, int32 PixelHeight)
{
	if (!CpuTexture || CpuTexture->GetSizeX() != PixelWidth || CpuTexture->GetSizeY() != PixelHeight)
	{
		CpuTexture = UTexture2D::CreateTransient(PixelWidth, PixelHeight, PF_R8G8B8A8);
		CpuTexture->SRGB = false;  // like the GPU path
		CpuTexture->Filter = TF_Bilinear;
		CpuTexture->UpdateResource();
		bRepaint = true;
	}
	JsuiRect Dirty;
	if (!Host->RenderCpu(PixelWidth, PixelHeight, bRepaint, Dirty)) return;
	bRepaint = false;
	// Upload the changed rectangle (a copy: the render thread reads it later).
	const int32 RowBytes = Dirty.width * 4;
	uint8* Copy = new uint8[size_t(RowBytes) * Dirty.height];
	const uint8* Src = Host->Pixels();
	for (int32 Y = 0; Y < Dirty.height; Y++)
	{
		FMemory::Memcpy(Copy + size_t(Y) * RowBytes, Src + (size_t(Dirty.y + Y) * PixelWidth + Dirty.x) * 4, RowBytes);
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(Dirty.x, Dirty.y, 0, 0, Dirty.width, Dirty.height);
	CpuTexture->UpdateTextureRegions(0, 1, Region, RowBytes, 4, Copy,
		[](uint8* Data, const FUpdateTextureRegion2D* R)
		{
			delete[] Data;
			delete R;
		});
}

int32 SJsuiView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
                         FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
                         bool bParentEnabled) const
{
	UObject* Texture = bGpu ? static_cast<UObject*>(Target.Get()) : static_cast<UObject*>(CpuTexture.Get());
	if (!Texture) return LayerId;
	Brush.SetResourceObject(Texture);
	Brush.ImageSize = AllottedGeometry.GetLocalSize();
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), &Brush,
	                           ESlateDrawEffect::PreMultipliedAlpha | ESlateDrawEffect::NoGamma, FLinearColor::White);
	return LayerId + 1;
}

void SJsuiView::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Target);
	Collector.AddReferencedObject(CpuTexture);
}

// ---------------------------------------------------------- input

bool SJsuiView::HandleMouseMove(const FPointerEvent& Event)
{
	if (!Host || !bForwardInput || !bLoaded) return false;
	const bool bInside = InsideView(Event.GetScreenSpacePosition());
	const FVector2D P = AbsoluteToUi(Event.GetScreenSpacePosition());
	if (bInside && P != Mouse)
	{
		jsui_send_mouse_move(Runtime(), P.X, P.Y);
		Mouse = P;
	}
	else if (!bInside && bMouseInside && ButtonsDown.Num() == 0)
	{
		jsui_send_mouse_leave(Runtime());
		Mouse = FVector2D(-1, -1);
	}
	bMouseInside = bInside;
	// A drag that started on the UI is the UI's.
	return bPointerCaptured;
}

bool SJsuiView::HandleMouseButton(const FPointerEvent& Event, bool bDown)
{
	JsuiMouseButton Button;
	const FKey Key = Event.GetEffectingButton();
	if (!Host || !bForwardInput || !bLoaded || !MouseButton(Key, Button)) return false;
	const FVector2D P = AbsoluteToUi(Event.GetScreenSpacePosition());
	if (bDown)
	{
		if (!InsideView(Event.GetScreenSpacePosition())) return false;
		ButtonsDown.Add(Key);
		const bool bConsumed = jsui_send_mouse_button(Runtime(), P.X, P.Y, Button, 1, Mods(Event.GetModifierKeys())) != 0;
		if (Key == EKeys::LeftMouseButton) bPointerCaptured = bConsumed;
		if (!bConsumed)
		{
			OnPassedMouseDown.Broadcast(Key, P);
			return false;
		}
		ButtonsConsumed.Add(Key);
		// Typed text needs keyboard focus (OnKeyChar).
		FSlateApplication::Get().SetAllUserFocus(AsShared(), EFocusCause::Mouse);
		return true;
	}
	if (ButtonsDown.Remove(Key) == 0) return false;
	jsui_send_mouse_button(Runtime(), P.X, P.Y, Button, 0, Mods(Event.GetModifierKeys()));
	if (Key == EKeys::LeftMouseButton) bPointerCaptured = false;
	return ButtonsConsumed.Remove(Key) > 0;
}

bool SJsuiView::HandleWheel(const FPointerEvent& Event)
{
	if (!Host || !bForwardInput || !bLoaded || !InsideView(Event.GetScreenSpacePosition())) return false;
	const FVector2D P = AbsoluteToUi(Event.GetScreenSpacePosition());
	const float Delta = Event.GetWheelDelta();
	// DOM: positive deltaY scrolls down, ~40 px per notch.
	if (jsui_send_mouse_wheel(Runtime(), P.X, P.Y, 0.f, -Delta * 40.f) != 0) return true;
	OnPassedWheel.Broadcast(Delta);
	return false;
}

bool SJsuiView::HandleKey(const FKeyEvent& Event, bool bDown)
{
	const FKey Key = Event.GetKey();
	if (!Host || !bForwardInput || !bLoaded || Key.IsMouseButton() || Key.IsGamepadKey()) return false;
	const FModifierKeysState& M = Event.GetModifierKeys();
	const FTCHARToUTF8 Code(*JsuiKeys::DomCode(Key));
	const FTCHARToUTF8 Name(*JsuiKeys::DomKey(Key, M.IsShiftDown()));
	if (bDown)
	{
		const bool bRepeat = Event.IsRepeat() || KeysDown.Contains(Key);
		KeysDown.Add(Key);
		const bool bConsumed = jsui_send_key(Runtime(), Name.Get(), Code.Get(), 1, bRepeat ? 1 : 0, Mods(M)) != 0;
		if (bConsumed)
		{
			KeysConsumed.Add(Key);
			return true;
		}
		if (!bRepeat) OnPassedKeyDown.Broadcast(Key);
		return false;
	}
	KeysDown.Remove(Key);
	jsui_send_key(Runtime(), Name.Get(), Code.Get(), 0, 0, Mods(M));
	return KeysConsumed.Remove(Key) > 0;
}

FReply SJsuiView::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	const TCHAR C = InCharacterEvent.GetCharacter();
	// Control characters are keys, not text.
	if (!Host || !bForwardInput || !bLoaded || C < 32 || C == 127) return FReply::Unhandled();
	const FString Text(1, &C);
	jsui_send_text(Runtime(), TCHAR_TO_UTF8(*Text));
	return FReply::Handled();
}

void SJsuiView::OnActivationChanged(bool bActive)
{
	// Keys held when the app lost focus never see their key up.
	if (!bActive) ReleaseKeys();
}

void SJsuiView::ReleaseKeys()
{
	if (!Host) return;
	for (const FKey& Key : KeysDown)
	{
		jsui_send_key(Runtime(), TCHAR_TO_UTF8(*JsuiKeys::DomKey(Key, false)), TCHAR_TO_UTF8(*JsuiKeys::DomCode(Key)), 0, 0, 0);
	}
	KeysDown.Reset();
	KeysConsumed.Reset();
}
