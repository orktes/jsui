// UJsuiWidget: SJsuiView for UMG. Add it to the viewport (or any UMG layout)
// to draw a JSUI bundle over the game; see SJsuiView.h for how it renders and
// forwards input.
//
//   UJsuiWidget* Ui = NewObject<UJsuiWidget>(this);
//   Ui->Bundle = TEXT("Jsui/bundle.js");      // Content/Jsui, staged as a non-UFS file
//   Ui->OnMessageNative.AddUObject(this, &AMyGame::OnUiMessage);
//   Ui->AddToViewport();                       // UJsuiWidget is a UWidget: see AddToViewport below
//   Ui->SetState(TEXT("game"), TEXT("{\"hp\":100}"));
#pragma once

#include "Components/Widget.h"
#include "CoreMinimal.h"
#include "SJsuiView.h"

#include "JsuiWidget.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJsuiMessageEvent, const FString&, Name, const FString&, Json);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJsuiMouseEvent, FKey, Button, FVector2D, Position);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJsuiWheelEvent, float, Delta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJsuiKeyEvent, FKey, Key);

UCLASS(meta = (DisplayName = "JSUI View"))
class JSUI_API UJsuiWidget : public UWidget
{
	GENERATED_BODY()

public:
	/** Bundle path relative to the project's Content dir, an absolute path, or an http:// URL (jsui dev). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JSUI")
	FString Bundle = TEXT("Jsui/bundle.js");
	/** Poll the dev server and hot-update (use with an http:// bundle from `npx jsui dev`). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JSUI")
	bool bDevMode = false;
	/** Rasterise on the CPU even where the GPU path (Metal) is available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JSUI")
	bool bForceCpu = false;
	/** Forward mouse and keyboard input to the UI. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JSUI")
	bool bForwardInput = true;
	/** Device pixels per logical px; 0: the screen's DPI scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "JSUI")
	float PixelRatio = 0.f;

	/** Host.postMessage(name, payload) from JS: name and the payload as JSON. */
	UPROPERTY(BlueprintAssignable, Category = "JSUI")
	FJsuiMessageEvent OnMessage;
	/** A mouse press the UI did not consume (position in UI px). */
	UPROPERTY(BlueprintAssignable, Category = "JSUI")
	FJsuiMouseEvent PassedMouseDown;
	/** A wheel movement the UI did not consume (notches). */
	UPROPERTY(BlueprintAssignable, Category = "JSUI")
	FJsuiWheelEvent PassedWheel;
	/** A key press the UI did not consume. */
	UPROPERTY(BlueprintAssignable, Category = "JSUI")
	FJsuiKeyEvent PassedKeyDown;

	/** Native versions of the events above (no UFUNCTION needed). */
	TMulticastDelegate<void(const FString&, const FString&)> OnMessageNative;
	TMulticastDelegate<void(const FKey&, const FVector2D&)> PassedMouseDownNative;
	TMulticastDelegate<void(float)> PassedWheelNative;
	TMulticastDelegate<void(const FKey&)> PassedKeyDownNative;

	/** useHostState(key) in JS. */
	UFUNCTION(BlueprintCallable, Category = "JSUI")
	void SetState(const FString& Key, const FString& Json);
	/** Host.addListener(name) in JS. */
	UFUNCTION(BlueprintCallable, Category = "JSUI")
	void PostMessage(const FString& Name, const FString& Json);
	/** Writes a frame buffer (Float32Array, useHostBufferValue in JS). */
	UFUNCTION(BlueprintCallable, Category = "JSUI")
	void SetBuffer(const FString& Key, const TArray<float>& Values);
	void SetBuffer(const FString& Key, const float* Values, int32 Count);

	/** Viewport size in UI (logical) px. */
	UFUNCTION(BlueprintPure, Category = "JSUI")
	FVector2D GetUiSize() const;
	UFUNCTION(BlueprintPure, Category = "JSUI")
	float GetUiPixelRatio() const;
	/** How the UI is drawn, e.g. "Skia Graphite · Metal · RenderTarget2D". */
	UFUNCTION(BlueprintPure, Category = "JSUI")
	FString GetRendererName() const;
	/** Viewport pixels (e.g. from a world-to-screen projection) to UI px, for a view covering the viewport. */
	UFUNCTION(BlueprintPure, Category = "JSUI")
	FVector2D ViewportToUi(FVector2D ViewportPixels) const;

	/** Adds the view to the game viewport, over the game, and gives it keyboard focus. */
	UFUNCTION(BlueprintCallable, Category = "JSUI")
	void AddToViewport(int32 ZOrder = 100);
	void RemoveFromViewport();

	TSharedPtr<SJsuiView> GetView() const { return View; }
	/** The runtime, for direct jsui_* calls (waits for a queued GPU render first). */
	JsuiRuntime* GetRuntime() const;

	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	TSharedPtr<SJsuiView> View;
	TSharedPtr<SWidget> ViewportContent;
};
