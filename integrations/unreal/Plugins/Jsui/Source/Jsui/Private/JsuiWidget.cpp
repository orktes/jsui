#include "JsuiWidget.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"

TSharedRef<SWidget> UJsuiWidget::RebuildWidget()
{
	View = SNew(SJsuiView)
		.Bundle(Bundle)
		.bDevMode(bDevMode)
		.bForceCpu(bForceCpu)
		.bForwardInput(bForwardInput)
		.PixelRatio(PixelRatio);
	TWeakObjectPtr<UJsuiWidget> Self(this);
	View->OnMessage.AddLambda([Self](const FString& Name, const FString& Json)
	{
		if (!Self.IsValid()) return;
		Self->OnMessageNative.Broadcast(Name, Json);
		Self->OnMessage.Broadcast(Name, Json);
	});
	View->OnPassedMouseDown.AddLambda([Self](const FKey& Button, const FVector2D& Position)
	{
		if (!Self.IsValid()) return;
		Self->PassedMouseDownNative.Broadcast(Button, Position);
		Self->PassedMouseDown.Broadcast(Button, Position);
	});
	View->OnPassedMouseUp.AddLambda([Self](const FKey& Button, const FVector2D& Position)
	{
		if (!Self.IsValid()) return;
		Self->PassedMouseUpNative.Broadcast(Button, Position);
		Self->PassedMouseUp.Broadcast(Button, Position);
	});
	View->OnPassedWheel.AddLambda([Self](float Delta)
	{
		if (!Self.IsValid()) return;
		Self->PassedWheelNative.Broadcast(Delta);
		Self->PassedWheel.Broadcast(Delta);
	});
	View->OnPassedKeyDown.AddLambda([Self](const FKey& Key)
	{
		if (!Self.IsValid()) return;
		Self->PassedKeyDownNative.Broadcast(Key);
		Self->PassedKeyDown.Broadcast(Key);
	});
	return View.ToSharedRef();
}

void UJsuiWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	View.Reset();
}

void UJsuiWidget::SetState(const FString& Key, const FString& Json)
{
	if (View) View->SetState(Key, Json);
}

void UJsuiWidget::PostMessage(const FString& Name, const FString& Json)
{
	if (View) View->PostMessage(Name, Json);
}

void UJsuiWidget::SetBuffer(const FString& Key, const TArray<float>& Values) { SetBuffer(Key, Values.GetData(), Values.Num()); }

void UJsuiWidget::SetBuffer(const FString& Key, const float* Values, int32 Count)
{
	if (View) View->SetBuffer(Key, Values, Count);
}

FVector2D UJsuiWidget::GetUiSize() const { return View ? View->GetUiSize() : FVector2D::ZeroVector; }

float UJsuiWidget::GetUiPixelRatio() const { return View ? View->GetPixelRatio() : 1.f; }

FString UJsuiWidget::GetRendererName() const { return View ? View->GetRendererName() : FString(); }

FVector2D UJsuiWidget::ViewportToUi(FVector2D ViewportPixels) const { return ViewportPixels / GetUiPixelRatio(); }

JsuiRuntime* UJsuiWidget::GetRuntime() const { return View ? View->Runtime() : nullptr; }

void UJsuiWidget::AddToViewport(int32 ZOrder)
{
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
	if (!Viewport || ViewportContent) return;
	ViewportContent = TakeWidget();
	Viewport->AddViewportWidgetContent(ViewportContent.ToSharedRef(), ZOrder);
	if (FSlateApplication::IsInitialized()) FSlateApplication::Get().SetAllUserFocus(ViewportContent, EFocusCause::SetDirectly);
}

void UJsuiWidget::RemoveFromViewport()
{
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
	if (Viewport && ViewportContent) Viewport->RemoveViewportWidgetContent(ViewportContent.ToSharedRef());
	ViewportContent.Reset();
}
