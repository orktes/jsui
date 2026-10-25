#include "ExampleGame.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "JsuiExampleLog.h"
#include "JsuiWidget.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "SceneView.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/SWindow.h"
#include "RHICommandList.h"
#include "Rendering/SlateRenderer.h"
#include "Slate/SlateViewportProvider.h"

#if PLATFORM_WINDOWS
#include <stdlib.h>
#else
#include <unistd.h>
#endif

DEFINE_LOG_CATEGORY(LogJsuiExample);

void JsuiExampleSay(const TCHAR* Line, bool bError)
{
	if (bError)
	{
		UE_LOG(LogJsuiExample, Error, TEXT("%s"), Line);
	}
	else
	{
		UE_LOG(LogJsuiExample, Display, TEXT("%s"), Line);
	}
}

namespace
{
const FColor kHues[] = {
	FColor(242, 195, 91), FColor(79, 209, 255), FColor(178, 107, 255), FColor(74, 222, 128),
	FColor(255, 154, 60), FColor(79, 140, 255), FColor(255, 77, 106),
};
constexpr int32 kHueCount = UE_ARRAY_COUNT(kHues);
constexpr int32 kMaxUnits = 48;

// Unity-style coordinates of the other examples (metres, y up) -> Unreal
// (centimetres, z up).
FVector World(float X, float Y, float Z) { return FVector(X, Z, Y) * 100.f; }
}  // namespace

AJsuiExampleGame::AJsuiExampleGame()
{
	PrimaryActorTick.bCanEverTick = true;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->bConstrainAspectRatio = false;
	RootComponent = Camera;

	// Engine content, found here so the cooker knows the game needs it
	// (DefaultGame.ini also always cooks /Engine/BasicShapes).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Mat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = Cube.Object;
	PlaneMesh = Plane.Object;
	CylinderMesh = Cylinder.Object;
	BaseMaterial = Mat.Object;
}

void AJsuiExampleGame::ParseArgs()
{
	// Tokens of the command line; options take one or two dashes, values
	// come after "=" (Unreal reads a bare argument as the map to open:
	// --screenshot=/tmp/out.png, not --screenshot /tmp/out.png).
	const TCHAR* Cmd = FCommandLine::Get();
	FString Token;
	while (FParse::Token(Cmd, Token, false))
	{
		FString A = Token, Value;
		if (!A.StartsWith(TEXT("-"))) continue;
		A.RemoveFromStart(TEXT("-"));
		A.RemoveFromStart(TEXT("-"));
		if (int32 Eq; A.FindChar(TEXT('='), Eq))
		{
			Value = A.Mid(Eq + 1).TrimQuotes();
			A.LeftInline(Eq);
		}
		if (A == TEXT("selftest")) bSelfTest = true;
		else if (A == TEXT("frames")) Frames = FCString::Atoi64(*Value);
		else if (A == TEXT("screenshot")) Screenshot = Value;
		else if (A == TEXT("bundle")) BundleOverride = Value;
		else if (A == TEXT("dev")) bDev = true;
		else if (A == TEXT("cpu")) bCpu = true;
		else if (A == TEXT("no-ui")) bNoUi = true;
		else if (A == TEXT("bench")) Bench = FCString::Atod(*Value);
		else if (A == TEXT("bench-warmup")) BenchWarmup = FCString::Atod(*Value);
		else if (A == TEXT("js-thread"))
			JsThread = uint8(Value == TEXT("off") ? 0 : Value == TEXT("lockstep") ? 2 : 1);
	}
	if (!Screenshot.IsEmpty() && Frames < 0 && !bSelfTest) Frames = 120;
}

void AJsuiExampleGame::BeginPlay()
{
	Super::BeginPlay();
	ParseArgs();
	if (GEngine)
	{
		// Uncapped for deterministic runs (measuring), 120 otherwise (VSync
		// is off in DefaultEngine.ini: on macOS it only applies at startup).
		const bool bUncapped = Frames >= 0 || bSelfTest || Bench > 0;
		GEngine->Exec(GetWorld(), bUncapped ? TEXT("t.MaxFPS 0") : TEXT("t.MaxFPS 120"));
	}
	// Matte, like the other examples: BasicShapeMaterial has no roughness
	// parameter, and its specular greys the dark ground at grazing angles.
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->EngineShowFlags.SetSpecular(false);
	BuildScene();

	APlayerController* Pc = UGameplayStatics::GetPlayerController(this, 0);
	if (Pc)
	{
		Pc->SetViewTarget(this);
		Pc->bShowMouseCursor = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		Pc->SetInputMode(Mode);
	}

	if (bNoUi)
	{
		JsuiExampleSay(TEXT("UI: off (--no-ui)"));
		for (int32 I = 0; I < 8; I++) Spawn();
		return;
	}
	Ui = NewObject<UJsuiWidget>(this);
	Ui->bForceCpu = bCpu;
	Ui->bDevMode = bDev;
	Ui->JsThread = EJsuiJsThread(JsThread);
	if (!BundleOverride.IsEmpty()) Ui->Bundle = BundleOverride;
	Ui->OnMessageNative.AddUObject(this, &AJsuiExampleGame::OnUiMessage);
	Ui->PassedMouseDownNative.AddLambda([this](const FKey& Button, const FVector2D& Position)
	{
		if (Button != EKeys::LeftMouseButton) return;
		bDragging = true;  // the press is the game's: orbit while held, select if it was a click
		PressAt = LastMouse = Position;
		Travel = 0;
		Clicks++;
	});
	Ui->PassedMouseUpNative.AddLambda([this](const FKey& Button, const FVector2D& Position)
	{
		if (Button != EKeys::LeftMouseButton || !bDragging) return;
		bDragging = false;
		Travel = FMath::Max(Travel, float(FVector2D::Distance(Position, PressAt)));
		if (Travel <= 4.f) Select(Pick(Position));  // a click, not a drag
	});
	Ui->PassedWheelNative.AddLambda([this](float Delta)
	{
		Distance = FMath::Clamp(Distance * (Delta > 0 ? 0.9f : 1.1f), 6.f, 40.f);
		Wheel++;
	});
	Ui->PassedKeyDownNative.AddLambda([this](const FKey& Key)
	{
		if (Key == EKeys::SpaceBar) Spawn();
		else if (Key == EKeys::Escape) bPaused = !bPaused;
		else return;
		Keys++;
	});
	Ui->AddToViewport();
	if (bSelfTest) Test = MakeUnique<FJsuiSelfTest>();
	for (int32 I = 0; I < 8; I++) Spawn();
	// Before the bundle runs (the view loads it on its first Slate tick): its
	// first render sees the state.
	PublishState();
}

void AJsuiExampleGame::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Ui) Ui->RemoveFromViewport();
	Super::EndPlay(Reason);
}

// ------------------------------------------------------------ scene

UMaterialInstanceDynamic* AJsuiExampleGame::Material(const FColor& Srgb)
{
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	M->SetVectorParameterValue(TEXT("Color"), FLinearColor::FromSRGBColor(Srgb));
	return M;
}

UStaticMeshComponent* AJsuiExampleGame::AddMesh(UStaticMesh* Mesh, UMaterialInterface* Mat, const FVector& Position, const FVector& Scale)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetMobility(EComponentMobility::Movable);
	C->SetStaticMesh(Mesh);
	C->SetMaterial(0, Mat);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetupAttachment(nullptr);
	C->RegisterComponent();
	C->SetWorldLocationAndRotation(Position, FRotator::ZeroRotator);
	C->SetWorldScale3D(Scale);
	return C;
}

void AJsuiExampleGame::BuildScene()
{
	// Exposure and tone: fixed, no filmic curve, so the colours come out as
	// in the other examples (the scene is lit for exposure 1).
	FPostProcessSettings& Post = Camera->PostProcessSettings;
	Post.bOverride_AutoExposureMethod = true;
	Post.AutoExposureMethod = AEM_Manual;
	Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post.AutoExposureApplyPhysicalCameraExposure = false;
	Post.bOverride_AutoExposureBias = true;
	Post.AutoExposureBias = 0.f;
	Post.bOverride_ToneCurveAmount = true;
	Post.ToneCurveAmount = 0.f;
	Post.bOverride_BloomIntensity = true;
	Post.BloomIntensity = 0.f;
	Post.bOverride_VignetteIntensity = true;
	Post.VignetteIntensity = 0.f;

	// Key light, and a soft fill from the other side for the flat ambient
	// the other examples have.
	UDirectionalLightComponent* Sun = NewObject<UDirectionalLightComponent>(this);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetIntensity(3.6f);
	Sun->RegisterComponent();
	Sun->SetWorldRotation(FRotator(-50.f, -30.f, 0.f));
	UDirectionalLightComponent* Fill = NewObject<UDirectionalLightComponent>(this);
	Fill->SetMobility(EComponentMobility::Movable);
	Fill->SetIntensity(1.1f);
	Fill->SetLightColor(FLinearColor(0.62f, 0.7f, 1.f));
	Fill->SetCastShadows(false);
	Fill->ForwardShadingPriority = 0;
	Fill->RegisterComponent();
	Fill->SetWorldRotation(FRotator(-35.f, 150.f, 0.f));

	// The background colour of the other examples, as fog at infinity (the
	// scene is closer than where it starts).
	UExponentialHeightFogComponent* Fog = NewObject<UExponentialHeightFogComponent>(this);
	Fog->SetFogDensity(0.05f);
	Fog->SetFogHeightFalloff(0.001f);
	Fog->SetStartDistance(9000.f);
	Fog->SetFogInscatteringColor(FLinearColor::FromSRGBColor(FColor(9, 12, 22)) * 0.6f);
	Fog->RegisterComponent();

	AddMesh(PlaneMesh, Material(FColor(14, 19, 34)), World(0, 0, 0), FVector(40, 40, 1));
	AddMesh(CylinderMesh, Material(FColor(26, 36, 64)), World(0, 1.1f, 0), FVector(2.1f, 2.1f, 2.2f));
	for (const FColor& C : kHues) Hues.Add(Material(C));

	// Selection: the selected cube gets a material of its own, lit towards
	// white and pulsing, and a glowing disc under it.
	SelectedMaterial = Material(FColor::White);
	RingMaterial = Material(FColor::White);
	Ring = AddMesh(CylinderMesh, RingMaterial, FVector::ZeroVector, FVector(1));
	Ring->SetVisibility(false);
}

void AJsuiExampleGame::Select(int32 Index)
{
	if (Index != Selected)
	{
		JsuiExampleSay(Index >= 0 ? *FString::Printf(TEXT("selected: UNIT %02d"), Index + 1) : TEXT("selection cleared"));
	}
	if (Units.IsValidIndex(Selected)) Units[Selected].Body->SetMaterial(0, Hues[Units[Selected].Hue]);
	Selected = Index;
	if (Units.IsValidIndex(Selected))
	{
		Units[Selected].Body->SetMaterial(0, SelectedMaterial);
		const FLinearColor C = FLinearColor::FromSRGBColor(kHues[Units[Selected].Hue]);
		RingMaterial->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(C, FLinearColor::White, 0.25f));
	}
	Ring->SetVisibility(Units.IsValidIndex(Selected));
}

// The unit under `Ui` (UI px), nearest first: the click deprojected with this
// frame's camera, against each cube in its component's (rotated, scaled)
// space, where the engine's cube mesh spans -50..50 cm.
int32 AJsuiExampleGame::Pick(const FVector2D& At) const
{
	FMatrix ViewProj;
	FIntRect Rect;
	ViewProjection(ViewProj, Rect);
	FVector Origin, Direction;
	FSceneView::DeprojectScreenToWorld(At * Ui->GetUiPixelRatio(), Rect, ViewProj.InverseFast(), Origin, Direction);
	int32 Best = -1;
	double BestDistance = 0;
	for (int32 I = 0; I < Units.Num(); I++)
	{
		const FTransform T = Units[I].Body->GetComponentTransform();
		const FVector O = T.InverseTransformPosition(Origin), D = T.InverseTransformVector(Direction);
		double Near = -1e30, Far = 1e30;
		bool bHit = true;
		for (int32 A = 0; A < 3 && bHit; A++)
		{
			if (FMath::Abs(D[A]) < 1e-9)
			{
				bHit = FMath::Abs(O[A]) <= 50.0;
				continue;
			}
			double T1 = (-50.0 - O[A]) / D[A], T2 = (50.0 - O[A]) / D[A];
			if (T1 > T2) Swap(T1, T2);
			Near = FMath::Max(Near, T1);
			Far = FMath::Min(Far, T2);
			bHit = Near <= Far && Far >= 0;
		}
		if (!bHit) continue;
		const double HitDistance = FVector::Distance(Origin, T.TransformPosition(O + D * FMath::Max(Near, 0.0)));
		if (Best < 0 || HitDistance < BestDistance)
		{
			Best = I;
			BestDistance = HitDistance;
		}
	}
	return Best;
}

bool AJsuiExampleGame::UnitOnScreen(int32 Index, FVector2D& UiPos) const
{
	if (!Units.IsValidIndex(Index)) return false;
	FMatrix ViewProj;
	FIntRect Rect;
	ViewProjection(ViewProj, Rect);
	FVector2D Screen;
	if (!FSceneView::ProjectWorldToScreen(Units[Index].Body->GetComponentLocation(), Rect, ViewProj, Screen)) return false;
	UiPos = Ui->ViewportToUi(Screen);
	const FVector2D Size = Ui->GetUiSize();
	return UiPos.X >= 0 && UiPos.Y >= 0 && UiPos.X < Size.X && UiPos.Y < Size.Y;
}

void AJsuiExampleGame::Spawn()
{
	if (Units.Num() >= kMaxUnits)
	{
		if (Selected == 0) Select(-1);
		Units[0].Body->DestroyComponent();
		Units.RemoveAt(0);
		if (Selected > 0) Selected--;
	}
	FUnit U;
	U.Angle = FMath::FRandRange(0.f, 6.28f);
	U.Radius = 2.5f + FMath::FRandRange(0.f, 7.f);
	U.Speed = (FMath::FRand() < 0.5f ? 1.f : -1.f) * (0.2f + FMath::FRandRange(0.f, 0.6f));
	U.Height = 0.5f + FMath::FRandRange(0.f, 1.5f);
	U.Size = 0.6f + FMath::FRandRange(0.f, 0.6f);
	U.Phase = FMath::FRandRange(0.f, 6.28f);
	U.Hue = Spawned++ % kHueCount;
	U.Body = AddMesh(CubeMesh, Hues[U.Hue], UnitPos(U), FVector(U.Size));
	Units.Add(U);
}

void AJsuiExampleGame::Clear()
{
	Select(-1);
	for (FUnit& U : Units) U.Body->DestroyComponent();
	Units.Reset();
}

FVector AJsuiExampleGame::UnitPos(const FUnit& U) const
{
	return World(FMath::Cos(U.Angle) * U.Radius, U.Height + 0.25f * FMath::Sin(Time * 2.f + U.Phase), FMath::Sin(U.Angle) * U.Radius);
}

// ------------------------------------------------------------ frame

void AJsuiExampleGame::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Bench > 0) BenchFrame();
	if (!Ui && !bNoUi) return;
	TSharedPtr<SJsuiView> View = Ui ? Ui->GetView() : nullptr;

	// Orbit while a press the UI passed on is held (UI px).
	FSlateApplication& Slate = FSlateApplication::Get();
	const FVector2D Mouse = View ? View->AbsoluteToUi(Slate.GetCursorPos()) : FVector2D::ZeroVector;
	if (!Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton)) bDragging = false;
	if (bDragging)
	{
		const FVector2D D = Mouse - LastMouse;
		Yaw -= D.X * 0.006f;
		Pitch += D.Y * 0.006f;
		Travel = FMath::Max(Travel, float(FVector2D::Distance(Mouse, PressAt)));
	}
	LastMouse = Mouse;

	const float Dt = (Frames >= 0 || bSelfTest) ? 1.f / 60.f : DeltaSeconds;  // deterministic runs
	if (!bPaused)
	{
		Time += Dt;
		for (FUnit& U : Units) U.Angle += U.Speed * Dt;
	}
	UpdateCamera();
	for (const FUnit& U : Units)
	{
		U.Body->SetWorldLocationAndRotation(UnitPos(U), FRotator(0.f, -FMath::RadiansToDegrees(U.Angle), 0.f));
	}
	if (Units.IsValidIndex(Selected))
	{
		const FUnit& U = Units[Selected];
		const FLinearColor C = FLinearColor::FromSRGBColor(kHues[U.Hue]);
		SelectedMaterial->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(C, FLinearColor::White, 0.35f + 0.15f * FMath::Sin(Time * 6.f)));
		const FVector At = UnitPos(U);
		const float R = U.Size * (2.1f + 0.15f * FMath::Sin(Time * 4.f));
		Ring->SetWorldLocation(FVector(At.X, At.Y, 1.f));
		Ring->SetWorldScale3D(FVector(R, R, 0.01f));
	}

	if (!Ui) return;
	PublishFrameData();
	CountFrame(FPlatformTime::Seconds());
	PublishState();
	if (!bSaidRenderer && View && View->GetUiSize().X > 1)
	{
		bSaidRenderer = true;
		JsuiExampleSay(*FString::Printf(TEXT("renderer: %s"), *Ui->GetRendererName()));
	}

	Frame++;
	if (Test && View)
	{
		const int32 R = Test->Step(*View, *this);
		if (R != 0)
		{
			Test.Reset();
			Finish(R > 0 ? 0 : 1);
		}
	}
	if (Frames >= 0 && Frame >= Frames) Finish(0);
	PollCapture();
}

void AJsuiExampleGame::UpdateCamera()
{
	Pitch = FMath::Clamp(Pitch, 0.15f, 1.35f);
	const FVector Eye = World(FMath::Cos(Yaw) * FMath::Cos(Pitch), FMath::Sin(Pitch), FMath::Sin(Yaw) * FMath::Cos(Pitch)) * Distance;
	const FVector Target = World(0, 0.8f, 0);
	Camera->SetWorldLocationAndRotation(Eye, (Target - Eye).Rotation());
	// 50 degrees vertical like the other examples; Unreal's FOV is horizontal.
	FVector2D Size(16, 9);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
	const float Aspect = Size.Y > 0 ? Size.X / Size.Y : 16.f / 9.f;
	Camera->SetFieldOfView(FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(25.f)) * Aspect)));
}

void AJsuiExampleGame::CountFrame(double Now)
{
	if (FramesSeen++ < FpsWarmup) return;
	const int64 N = FramesSeen - FpsWarmup;  // frames in the ring, up to FpsWindow
	FrameTimes[(N - 1) % FpsWindow] = Now;
	if (N < 20) return;
	const double Oldest = FrameTimes[N < FpsWindow ? 0 : N % FpsWindow];
	const int64 Count = N < FpsWindow ? N : FpsWindow;
	if (Now <= Oldest || (FpsShownAt >= 0 && Now - FpsShownAt < 0.25)) return;
	Fps = int32(FMath::RoundToDouble((Count - 1) / (Now - Oldest)));
	FpsShownAt = Now;
}

// Unit screen positions for the nameplates: x, y, visible, hue (UI px). The
// projection is this frame's camera (the player camera manager's view is a
// frame behind at this point).
void AJsuiExampleGame::ViewProjection(FMatrix& Out, FIntRect& Rect) const
{
	FVector2D Viewport(1, 1);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Viewport);
	FMinimalViewInfo ViewInfo;
	Camera->GetCameraView(0.f, ViewInfo);
	ViewInfo.AspectRatio = Viewport.X / FMath::Max(1.0, Viewport.Y);
	FMatrix ViewMatrix, ProjectionMatrix;
	UGameplayStatics::GetViewProjectionMatrix(ViewInfo, ViewMatrix, ProjectionMatrix, Out);
	Rect = FIntRect(0, 0, FMath::RoundToInt(Viewport.X), FMath::RoundToInt(Viewport.Y));
}

void AJsuiExampleGame::PublishFrameData()
{
	FMatrix ViewProj;
	FIntRect Rect;
	ViewProjection(ViewProj, Rect);
	const FVector2D Size = Ui->GetUiSize();

	Entities.SetNumUninitialized(Units.Num() * 4);
	for (int32 I = 0; I < Units.Num(); I++)
	{
		const FUnit& U = Units[I];
		const FVector Head = UnitPos(U) + FVector(0, 0, (U.Size * 0.5f + 0.35f) * 100.f);
		FVector2D Screen;
		bool bVisible = FSceneView::ProjectWorldToScreen(Head, Rect, ViewProj, Screen);
		const FVector2D S = Ui->ViewportToUi(Screen);
		bVisible = bVisible && S.X > -60 && S.Y > -60 && S.X < Size.X + 60 && S.Y < Size.Y + 60;
		Entities[I * 4 + 0] = FMath::RoundToFloat(S.X);
		Entities[I * 4 + 1] = FMath::RoundToFloat(S.Y);
		Entities[I * 4 + 2] = bVisible ? 1.f : 0.f;
		Entities[I * 4 + 3] = float(I % kHueCount);
	}
	Ui->SetBuffer(TEXT("entities"), Entities.GetData(), Entities.Num());
}

void AJsuiExampleGame::PublishState()
{
	const FString Json = FString::Printf(
		TEXT("{\"engine\":\"Unreal %s\",\"renderer\":\"%s\",\"fps\":%d,\"entities\":%d,\"paused\":%s,"
		     "\"selected\":%d,\"passed\":{\"clicks\":%d,\"wheel\":%d,\"keys\":%d,\"pad\":0}}"),
		*FEngineVersion::Current().ToString(EVersionComponent::Patch), *Ui->GetRendererName(), Fps, Units.Num(),
		bPaused ? TEXT("true") : TEXT("false"), Selected, Clicks, Wheel, Keys);
	if (Json == LastState) return;
	LastState = Json;
	Ui->SetState(TEXT("game"), Json);
}

void AJsuiExampleGame::OnUiMessage(const FString& Name, const FString& Json)
{
	if (Test) Test->OnMessage(Name, Json);
	if (Name == TEXT("game:spawn"))
	{
		const int32 N = FMath::Clamp(int32(FJsuiSelfTest::JsonNumber(Json, TEXT("count"), 1)), 1, 50);
		for (int32 I = 0; I < N; I++) Spawn();
	}
	else if (Name == TEXT("game:clear"))
	{
		Clear();
	}
	else if (Name == TEXT("game:pause"))
	{
		bPaused = Json.Contains(TEXT("\"paused\":true"));
	}
	else if (Name == TEXT("game:quit"))
	{
		Finish(0);
	}
}

// ------------------------------------------------------------ exit

void AJsuiExampleGame::Finish(int32 Code)
{
	if (bFinishing) return;
	bFinishing = true;
	ExitCode = Code;
	if (!Screenshot.IsEmpty())
	{
		StartCapture();  // Tick quits once the frame is read back
		return;
	}
	Quit(Code);
}

// --bench: frame-to-frame times over a wall-clock window after a warm-up,
// then the same `bench:` line as the other engine examples (BenchTimer in
// integrations/shared/example_host.h).
void AJsuiExampleGame::BenchFrame()
{
	const double Now = FPlatformTime::Seconds();
	if (BenchStart < 0) BenchStart = Now;
	if (Now - BenchStart < BenchWarmup) return;
	if (BenchLast < 0) BenchFrom = Now;
	else BenchMs.Add(float((Now - BenchLast) * 1000.0));
	BenchLast = Now;
	if (Now - BenchFrom < Bench || BenchMs.Num() == 0) return;
	TArray<float> S = BenchMs;
	S.Sort();
	double Sum = 0;
	for (float V : BenchMs) Sum += V;
	const double Mean = Sum / BenchMs.Num();
	auto Q = [&](double F) { return S[FMath::Min(S.Num() - 1, int32(F * S.Num()))]; };
	const FString Label = Ui ? Ui->GetRendererName() : FString(TEXT("no-ui"));
	JsuiExampleSay(*FString::Printf(TEXT("bench: %s frames %d mean %.4f ms (%.1f fps) p50 %.4f p90 %.4f p99 %.4f max %.4f"),
		*Label, BenchMs.Num(), Mean, 1000.0 / Mean, Q(0.5), Q(0.9), Q(0.99), S.Last()));
	Bench = 0;
	Quit(0);
}

void AJsuiExampleGame::Quit(int32 Code)
{
	if (Code == 0)
	{
		FPlatformMisc::RequestExit(false);
		return;
	}
	// The exit status of RequestExitWithStatus is ignored on macOS: flush the
	// log and leave with it.
	GLog->Flush();
	fflush(stdout);
	_exit(Code);
}

// --screenshot: the frame as presented, scene + UI, read back from the
// window's back buffer when Slate has drawn into it (render thread).
// (FSlateApplication::TakeScreenshot redraws the window instead, over the
// game viewport's image, which is the back buffer still holding an earlier
// frame's UI: moving nameplates got ghosts.)
void AJsuiExampleGame::StartCapture()
{
	Capture = MakeShared<FCapture, ESPMode::ThreadSafe>();
	TSharedPtr<FCapture, ESPMode::ThreadSafe> C = Capture;
	CaptureHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddLambda(
		[C](SWindow&, ISlateViewportProvider& Provider)
		{
			if (C->bDone) return;
			FRHITexture* BackBuffer = Provider.GetBackBufferResource();
			if (!BackBuffer) return;
			const FIntPoint Size = BackBuffer->GetSizeXY();
			TArray<FColor> Pixels;
			FRHICommandListImmediate::Get().ReadSurfaceData(BackBuffer, FIntRect(FIntPoint::ZeroValue, Size), Pixels, FReadSurfaceDataFlags());
			FScopeLock Lock(&C->Mutex);
			C->Pixels = MoveTemp(Pixels);
			C->Size = Size;
			C->bDone = true;
		});
	CaptureWait = 0;
}

void AJsuiExampleGame::PollCapture()
{
	if (!Capture) return;
	if (!Capture->bDone)
	{
		if (++CaptureWait > 300)
		{
			JsuiExampleSay(TEXT("screenshot failed: no frame was presented"), true);
			Capture.Reset();
			Quit(1);
		}
		return;
	}
	FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(CaptureHandle);
	TArray<FColor> Pixels;
	FIntPoint Size;
	{
		FScopeLock Lock(&Capture->Mutex);
		Pixels = MoveTemp(Capture->Pixels);
		Size = Capture->Size;
	}
	Capture.Reset();
	for (FColor& Px : Pixels) Px.A = 255;
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
	// Plain stdio: the packaged game's file layer refuses paths outside the
	// game (e.g. /tmp).
	const FString Path = FPaths::ConvertRelativePathToFull(Screenshot);
	FILE* File = Png.Num() ? fopen(TCHAR_TO_UTF8(*Path), "wb") : nullptr;
	const bool bWritten = File && fwrite(Png.GetData(), 1, size_t(Png.Num()), File) == size_t(Png.Num());
	if (File) fclose(File);
	if (!bWritten)
	{
		JsuiExampleSay(*FString::Printf(TEXT("screenshot failed: could not write %s"), *Path), true);
		Quit(1);
		return;
	}
	JsuiExampleSay(*FString::Printf(TEXT("screenshot: %s (%dx%d)"), *Path, Size.X, Size.Y));
	Quit(ExitCode);
}

// ------------------------------------------------------------ game mode

AJsuiExampleGameMode::AJsuiExampleGameMode()
{
	DefaultPawnClass = nullptr;  // the game's camera is the view target
}

void AJsuiExampleGameMode::StartPlay()
{
	Super::StartPlay();
	GetWorld()->SpawnActor<AJsuiExampleGame>();
}
