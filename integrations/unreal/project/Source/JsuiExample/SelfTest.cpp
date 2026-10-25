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
	if (Name == TEXT("ui:anchors"))
	{
		SpawnX = float(JsonNumber(Json, TEXT("x")) + JsonNumber(Json, TEXT("width")) / 2);
		SpawnY = float(JsonNumber(Json, TEXT("y")) + JsonNumber(Json, TEXT("height")) / 2);
		bAnchors = true;
	}
	else if (Name == TEXT("ui:selected"))
	{
		Reported = int32(JsonNumber(Json, TEXT("index"), -1));
		Reports++;
	}
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

int32 FJsuiSelfTest::Step(SJsuiView& View, const IHost& Host)
{
	JsuiRuntime* Rt = View.Runtime();
	const FVector2D Size = View.GetUiSize();
	Frame++;
	switch (Phase)
	{
	case 0:  // wait for the UI and a few frames of layout
		if (bAnchors && Frame > 30) Phase = KStall;
		else if (Frame > 900) return Fail(TEXT("the UI never reported ui:anchors"));
		return 0;
	case KStall:
	{
		const int32 R = StallStep(Rt);
		if (R > 0) Phase = 1;
		return R < 0 ? -1 : 0;
	}
	case 1:
	{
		const FVector2D P(Size.X * 0.5f, Size.Y * 0.45f);
		if (jsui_hit_test(Rt, P.X, P.Y) != 0) return Fail(TEXT("jsui_hit_test reports UI over the empty middle of the screen"));
		Before = Host.GetEntities();
		ClicksBefore = Host.GetClicks();
		if (!Click(View, P)) return Fail(TEXT("the view has no native window"));
		// The pass-through delegate runs inside OnMouseDown.
		if (Host.GetClicks() != ClicksBefore + 1) return Fail(TEXT("a click on empty space did not reach the game"));
		if (Host.GetEntities() != Before) return Fail(TEXT("a click on empty space changed the units"));
		JsuiExampleSay(TEXT("selftest: empty-space click passed through to the game"));
		Phase = 2;
		return 0;
	}
	case 2:
	{
		if ((jsui_hit_test(Rt, SpawnX, SpawnY) & JSUI_HIT_POINTER) == 0) return Fail(TEXT("jsui_hit_test misses the Spawn button"));
		Before = Host.GetEntities();
		ClicksBefore = Host.GetClicks();
		if (!Click(View, FVector2D(SpawnX, SpawnY))) return Fail(TEXT("the view has no native window"));
		if (Host.GetClicks() != ClicksBefore) return Fail(TEXT("the click on the Spawn button was passed to the game"));
		JsuiExampleSay(TEXT("selftest: Spawn button click consumed by the UI"));
		Phase = 3;
		Wait = Frame;
		return 0;
	}
	case 3:
		if (Host.GetEntities() == Before + 1)
		{
			JsuiExampleSay(*FString::Printf(TEXT("selftest: game:spawn reached the game (%d -> %d units)"), Before, Host.GetEntities()));
			Phase = 4;
			return 0;
		}
		if (Frame - Wait > 120) return Fail(TEXT("game:spawn never arrived"));
		return 0;
	case 4:  // select a unit, clear, select again
	{
		FVector2D P;
		int32 Want = -1;
		if (Round == 1)
		{
			if (!EmptyGround(Rt, Host, Size, P)) return Fail(TEXT("no empty ground to click"));
		}
		else
		{
			Want = ClickableUnit(Rt, Host, Host.GetEntities(), Size, P);
			if (Want < 0) return Fail(TEXT("no unit on screen to click"));
		}
		if (!Click(View, P)) return Fail(TEXT("the view has no native window"));
		if (Host.GetSelected() != Want)
		{
			JsuiExampleSay(*FString::Printf(TEXT("selftest: clicked (%.0f, %.0f): selected %d, expected %d"), P.X, P.Y, Host.GetSelected(), Want), true);
			return Fail(Want < 0 ? TEXT("a click on empty ground did not clear the selection") : TEXT("a click on a unit did not select it"));
		}
		Expect = Want;
		ReportsBefore = Reports;
		Phase = 5;
		Wait = Frame;
		return 0;
	}
	case 5:
		if (Reports > ReportsBefore && Reported == Expect)
		{
			JsuiExampleSay(Expect >= 0
				? *FString::Printf(TEXT("selftest: a click on unit %d selected it; the UI reports it"), Expect)
				: TEXT("selftest: a click on empty ground cleared the selection; the UI reports it"));
			if (++Round < 3)
			{
				Phase = 4;
			}
			else
			{
				Phase = 6;
				Wait = Frame;
			}
			return 0;
		}
		if (Frame - Wait > 120) return Fail(TEXT("the UI never reported the selection (ui:selected)"));
		return 0;
	default:
		// A few more frames so the selection is on screen for --screenshot.
		if (Frame - Wait < 20) return 0;
		JsuiExampleSay(TEXT("selftest: ok"));
		return 1;
	}
}

// A unit whose centre is on screen, away from the edges, not under the UI,
// and picked there (not hidden behind another one). The newest first.
int32 FJsuiSelfTest::ClickableUnit(JsuiRuntime* Rt, const IHost& Host, int32 Count, const FVector2D& Size, FVector2D& P) const
{
	for (int32 I = Count - 1; I >= 0; I--)
	{
		if (!Host.UnitOnScreen(I, P)) continue;
		if (P.X < 40 || P.Y < 40 || P.X > Size.X - 40 || P.Y > Size.Y - 40) continue;
		if (jsui_hit_test(Rt, P.X, P.Y) != 0) continue;
		if (Host.Pick(P) == I) return I;
	}
	return -1;
}

// A point with neither UI nor a unit under it.
bool FJsuiSelfTest::EmptyGround(JsuiRuntime* Rt, const IHost& Host, const FVector2D& Size, FVector2D& P) const
{
	for (int32 GY = 2; GY < 10; GY++)
	{
		for (int32 GX = 1; GX < 16; GX++)
		{
			P = FVector2D(Size.X * GX / 16.0, Size.Y * GY / 12.0);
			if (jsui_hit_test(Rt, P.X, P.Y) == 0 && Host.Pick(P) < 0) return true;
		}
	}
	return false;
}

// With a JS thread, posting the stall returns at once, no gap between two
// game frames comes near it while it runs, and jsui_sync finds JS idle after
// it. Off or in lockstep the post itself holds the game for the stall.
int32 FJsuiSelfTest::StallStep(JsuiRuntime* Rt)
{
	const double Now = FPlatformTime::Seconds() * 1000.0;
	if (StallFrames == 0)
	{
		const int32 Mode = jsui_js_thread(Rt);
		jsui_debug_stall_js(Rt, KStallMs);
		const double Held = FPlatformTime::Seconds() * 1000.0 - Now;
		if (Mode != JSUI_JS_THREAD_ON)
		{
			if (Held < KStallMs * 0.9) return Fail(TEXT("jsui_debug_stall_js did not keep JS busy"));
			JsuiExampleSay(*FString::Printf(TEXT("selftest: JS thread %s: a %.0f ms JS stall held the game for %.0f ms"),
			       Mode == JSUI_JS_THREAD_LOCKSTEP ? TEXT("lockstep") : TEXT("off"), KStallMs, Held));
			return 1;
		}
		if (Held > KStallMs / 4) return Fail(TEXT("posting a JS stall waited for the JS thread"));
		StallStart = LastFrame = FPlatformTime::Seconds() * 1000.0;
		StallFrames = 1;
		return 0;
	}
	const double Gap = Now - LastFrame;
	LastFrame = Now;
	MaxGap = FMath::Max(MaxGap, Gap);
	if (Now - StallStart < KStallMs)
	{
		if (Gap > KStallMs / 2) return Fail(TEXT("a game frame waited for the busy JS thread"));
		StallFrames++;
		return 0;
	}
	if (StallFrames < 3) return Fail(TEXT("too few game frames while JS was busy"));
	if (jsui_sync(Rt, 2000) != 1) return Fail(TEXT("JS did not come back from its stall"));
	JsuiExampleSay(*FString::Printf(TEXT("selftest: JS thread on: %d game frames during a %.0f ms JS stall, the longest %.1f ms"),
	       StallFrames, KStallMs, MaxGap));
	return 1;
}

int32 FJsuiSelfTest::Fail(const TCHAR* What)
{
	JsuiExampleSay(*FString::Printf(TEXT("selftest: FAILED: %s"), What), true);
	return -1;
}
