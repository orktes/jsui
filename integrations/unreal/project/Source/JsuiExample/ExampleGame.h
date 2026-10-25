// The Unreal version of the engine examples' game (integrations/raylib, sdl3,
// sokol, unity): a small 3D scene with the shared HUD
// (integrations/shared/ui/app.jsx) on top, through UJsuiWidget.
//
// Everything is built in code from engine content (/Engine/BasicShapes), so
// the project has no assets: the game mode spawns this actor into the
// engine's empty Entry map.
//
// Input pass-through: UJsuiWidget raises PassedMouseDown / PassedMouseUp /
// PassedWheel / PassedKeyDown only for input the UI did not consume. Click a
// unit to select it (the deprojected click against each cube) or empty
// ground to clear the selection, drag empty space to orbit the camera, wheel
// to zoom, Space spawns a unit, Esc pauses.
//
// Per-frame data: every unit is projected to the screen each frame and
// written as x, y, visible, hue into the `entities` frame buffer, which moves
// the nameplates without running JS; fps, unit count, the selected unit and
// the pass-through counters go to the `game` shared state.
//
// Command line: --selftest, --frames=N, --screenshot=out.png,
// --bundle=path-or-url, --dev, --cpu (the CPU renderer even on Metal),
// --js-thread=on|off|lockstep (UJsuiWidget::JsThread; on by default).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "SelfTest.h"

#include <atomic>

#include "ExampleGame.generated.h"

class UCameraComponent;
class UJsuiWidget;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

UCLASS()
class AJsuiExampleGame : public AActor, public FJsuiSelfTest::IHost
{
	GENERATED_BODY()

public:
	AJsuiExampleGame();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	// FJsuiSelfTest::IHost
	virtual int32 GetEntities() const override { return Units.Num(); }
	virtual int32 GetClicks() const override { return Clicks; }
	virtual int32 Pick(const FVector2D& At) const override;
	virtual bool UnitOnScreen(int32 Index, FVector2D& UiPos) const override;
	virtual int32 GetSelected() const override { return Selected; }

private:
	struct FUnit
	{
		float Angle, Radius, Speed, Height, Size, Phase;
		int32 Hue;
		UStaticMeshComponent* Body;  // owned by this actor
	};

	void ParseArgs();
	void BuildScene();
	UStaticMeshComponent* AddMesh(UStaticMesh* Mesh, UMaterialInterface* Material, const FVector& Position, const FVector& Scale);
	UMaterialInstanceDynamic* Material(const FColor& Srgb);
	void Spawn();
	void Clear();
	FVector UnitPos(const FUnit& U) const;
	void UpdateCamera();
	// This frame's camera: view-projection and the viewport rectangle (px).
	void ViewProjection(FMatrix& Out, FIntRect& Rect) const;
	void Select(int32 Index);
	void PublishFrameData();
	void PublishState();
	void CountFrame(double Now);
	void OnUiMessage(const FString& Name, const FString& Json);
	void Finish(int32 Code);
	void Quit(int32 Code);
	void StartCapture();
	void PollCapture();

	UPROPERTY()
	TObjectPtr<UJsuiWidget> Ui;
	UPROPERTY()
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY()
	TObjectPtr<UStaticMesh> PlaneMesh;
	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Hues;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SelectedMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> RingMaterial;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Ring;

	TArray<FUnit> Units;
	TArray<float> Entities;
	int32 Spawned = 0;
	float Time = 0, Yaw = 0.8f, Pitch = 0.55f, Distance = 16.f;
	bool bPaused = false, bDragging = false;
	FVector2D LastMouse = FVector2D::ZeroVector;
	FVector2D PressAt = FVector2D::ZeroVector;  // UI px
	float Travel = 0;                           // how far the pointer went since the press
	int32 Selected = -1;
	int32 Clicks = 0, Wheel = 0, Keys = 0;
	// FPS over the last 60 frames after a 10-frame warm-up, shown at most
	// every 250 ms (FpsCounter in integrations/shared/example_host.h).
	static constexpr int32 FpsWindow = 60, FpsWarmup = 10;
	double FrameTimes[FpsWindow] = {};
	int64 FramesSeen = 0;
	double FpsShownAt = -1;
	int32 Fps = 0;
	FString LastState;
	bool bSaidRenderer = false;

	// Command line.
	bool bSelfTest = false, bDev = false, bCpu = false;
	uint8 JsThread = 1;  // --js-thread=on|off|lockstep: EJsuiJsThread
	int64 Frames = -1, Frame = 0;
	FString Screenshot, BundleOverride;
	TUniquePtr<FJsuiSelfTest> Test;
	bool bFinishing = false;
	int32 ExitCode = 0;

	// --screenshot readback (filled on the render thread).
	struct FCapture
	{
		FCriticalSection Mutex;
		std::atomic<bool> bDone{false};
		TArray<FColor> Pixels;
		FIntPoint Size;
	};
	TSharedPtr<FCapture, ESPMode::ThreadSafe> Capture;
	FDelegateHandle CaptureHandle;
	int32 CaptureWait = 0;
};

UCLASS()
class AJsuiExampleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AJsuiExampleGameMode();
	virtual void StartPlay() override;
};
