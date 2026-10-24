#include "SelfTest.h"

#include "Framework/Application/SlateApplication.h"
#include "JsuiExampleLog.h"
#include "SJsuiView.h"
#include "Widgets/SWindow.h"

THIRD_PARTY_INCLUDES_START
#include "jsui/jsui.h"
THIRD_PARTY_INCLUDES_END

double FJsuiSelfTest::JsonNumber(const FString& Json, const TCHAR* Key, double Fallback)
{
	const FString K = FString::Printf(TEXT("\"%s\":"), Key);
	int32 I = Json.Find(K, ESearchCase::CaseSensitive);
	if (I == INDEX_NONE) return Fallback;
	I += K.Len();
	int32 J = I;
	while (J < Json.Len() && FCString::Strchr(TEXT("+-.0123456789eE"), Json[J])) J++;
	return J > I ? FCString::Atod(*Json.Mid(I, J - I)) : Fallback;
}

void FJsuiSelfTest::OnMessage(const FString& Name, const FString& Json)
{
	if (Name != TEXT("ui:anchors")) return;
	SpawnX = float(JsonNumber(Json, TEXT("x")) + JsonNumber(Json, TEXT("width")) / 2);
	SpawnY = float(JsonNumber(Json, TEXT("y")) + JsonNumber(Json, TEXT("height")) / 2);
	bAnchors = true;
}

bool FJsuiSelfTest::Click(SJsuiView& View, const FVector2D& Ui)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(View.AsShared());
	const TSharedPtr<FGenericWindow> Native = Window ? Window->GetNativeWindow() : nullptr;
	if (!Native) return false;
	// The view's offset in its window (Slate absolute px) ...
	const FVector2D InWindow = View.GetTickSpaceGeometry().GetAbsolutePosition() - Window->GetPositionInScreen();
	// ... and where the window really is on the desktop: on macOS the Cocoa
	// frame (Slate's cached position is wrong for fullscreen windows there),
	// elsewhere Slate's position, which tracks the OS's window messages.
	FVector2D Origin = Window->GetPositionInScreen();
#if PLATFORM_MAC
	int32 X, Y, W, H;
	// GetRestoredDimensions reads the NSWindow frame (its top one px low);
	// the content is the bottom of the frame, under the title bar.
	if (Native->GetRestoredDimensions(X, Y, W, H)) Origin = FVector2D(X, Y - 1 + H - Window->GetSizeInScreen().Y);
#endif
	const FVector2D P = Origin + InWindow + Ui * View.GetPixelRatio();
	// What FMacApplication / FWindowsApplication do: the cursor moves (the
	// handler reads its position back), then the button goes down and up.
	Slate.SetCursorPos(P);
	Slate.OnMouseMove();
	Slate.OnMouseDown(Native, EMouseButtons::Left, P);
	Slate.OnMouseUp(EMouseButtons::Left, P);
	return true;
}

int32 FJsuiSelfTest::Step(SJsuiView& View, int32 Entities, int32 Clicks)
{
	JsuiRuntime* Rt = View.Runtime();
	const FVector2D Size = View.GetUiSize();
	Frame++;
	switch (Phase)
	{
	case 0:  // wait for the UI and a few frames of layout
		if (bAnchors && Frame > 30) Phase = 1;
		else if (Frame > 900) return Fail(TEXT("the UI never reported ui:anchors"));
		return 0;
	case 1:
	{
		const FVector2D P(Size.X * 0.5f, Size.Y * 0.45f);
		if (jsui_hit_test(Rt, P.X, P.Y) != 0) return Fail(TEXT("jsui_hit_test reports UI over the empty middle of the screen"));
		Before = Entities;
		ClicksBefore = Clicks;
		if (!Click(View, P)) return Fail(TEXT("the view has no native window"));
		Phase = 2;
		return 0;
	}
	case 2:
		// The pass-through delegate runs inside OnMouseDown: already counted.
		if (Clicks != ClicksBefore + 1) return Fail(TEXT("a click on empty space did not reach the game"));
		if (Entities != Before) return Fail(TEXT("a click on empty space changed the units"));
		JsuiExampleSay(TEXT("selftest: empty-space click passed through to the game"));
		Phase = 3;
		return 0;
	case 3:
	{
		if ((jsui_hit_test(Rt, SpawnX, SpawnY) & JSUI_HIT_POINTER) == 0) return Fail(TEXT("jsui_hit_test misses the Spawn button"));
		Before = Entities;
		ClicksBefore = Clicks;
		if (!Click(View, FVector2D(SpawnX, SpawnY))) return Fail(TEXT("the view has no native window"));
		if (Clicks != ClicksBefore) return Fail(TEXT("the click on the Spawn button was passed to the game"));
		JsuiExampleSay(TEXT("selftest: Spawn button click consumed by the UI"));
		Phase = 4;
		Wait = Frame;
		return 0;
	}
	case 4:
		if (Entities == Before + 1)
		{
			JsuiExampleSay(*FString::Printf(TEXT("selftest: game:spawn reached the game (%d -> %d units)"), Before, Entities));
			Phase = 5;
			Wait = Frame;
			return 0;
		}
		if (Frame - Wait > 120) return Fail(TEXT("game:spawn never arrived"));
		return 0;
	default:
		// A few more frames so the new unit's nameplate is on screen for
		// --screenshot.
		if (Frame - Wait < 20) return 0;
		JsuiExampleSay(TEXT("selftest: ok"));
		return 1;
	}
}

int32 FJsuiSelfTest::Fail(const TCHAR* What)
{
	JsuiExampleSay(*FString::Printf(TEXT("selftest: FAILED: %s"), What), true);
	return -1;
}
