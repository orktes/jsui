// --selftest: the C++ port of SelfTest in integrations/shared/example_host.h
// (and the Unity example's SelfTest.cs). Once the UI has reported where its
// Spawn button is, it stalls JS for a moment, then clicks like the OS does:
// FSlateApplication's platform message handler (OnMouseMove / OnMouseDown /
// OnMouseUp) with the window and desktop coordinates taken from the native
// window, not from Slate's idea of where the window is. So the clicks take
// the same path as real ones (input preprocessors, SJsuiView's mapping to UI
// px, Slate routing). It checks:
//   0. with JS on its own thread (UJsuiWidget::JsThread On, the default), a
//      JS stall (jsui_debug_stall_js) holds no game frame: the frames go on
//      meanwhile and JS catches up after it;
// and the pass-through contract:
//   1. a click on empty space (the middle of the screen) is not consumed:
//      jsui_hit_test says nothing is there and the game sees the click;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the game, which adds a unit;
//   3. a click on a unit selects it, and the UI reports the selection back
//      (ui:selected); a click on empty ground clears it; another click
//      selects again (so --screenshot shows a selected unit).
// Logs "selftest: ok" (exit 0) or what failed (exit 1).
#pragma once

#include "CoreMinimal.h"

class SJsuiView;
struct JsuiRuntime;

class FJsuiSelfTest
{
public:
	// What the test reads from the game.
	class IHost
	{
	public:
		virtual ~IHost() = default;
		virtual int32 GetEntities() const = 0;
		// Presses the UI passed on to the game.
		virtual int32 GetClicks() const = 0;
		// The unit a click at `Ui` (logical px) selects, or -1.
		virtual int32 Pick(const FVector2D& Ui) const = 0;
		// Unit `Index`'s centre on screen (logical px); false when off screen.
		virtual bool UnitOnScreen(int32 Index, FVector2D& Ui) const = 0;
		virtual int32 GetSelected() const = 0;
	};

	// The number after "key": in a flat JSON object (messages of app.jsx are
	// tiny and fixed; a real game would use its JSON library).
	static double JsonNumber(const FString& Json, const TCHAR* Key, double Fallback = 0);

	void OnMessage(const FString& Name, const FString& Json);
	// Once per frame. Returns 0 while running, 1 when passed, -1 when failed.
	int32 Step(SJsuiView& View, const IHost& Host);

private:
	static int32 Fail(const TCHAR* What);
	// A left click at UI coordinates, through FSlateApplication as the OS
	// sends it. False when the view is not in a window yet.
	static bool Click(SJsuiView& View, const FVector2D& Ui);
	int32 ClickableUnit(JsuiRuntime* Rt, const IHost& Host, int32 Count, const FVector2D& Size, FVector2D& P) const;
	bool EmptyGround(JsuiRuntime* Rt, const IHost& Host, const FVector2D& Size, FVector2D& P) const;
	// One JS stall, stepped once per frame: 1 done, -1 failed, 0 running.
	int32 StallStep(JsuiRuntime* Rt);

	static constexpr int32 KStall = 100;
	static constexpr double KStallMs = 300;
	int32 StallFrames = 0;
	double StallStart = 0, LastFrame = 0, MaxGap = 0;  // ms

	bool bAnchors = false;
	float SpawnX = 0, SpawnY = 0;
	int32 Phase = 0, Before = 0, ClicksBefore = 0, Round = 0, Expect = -1;
	int32 Reported = -1, Reports = 0, ReportsBefore = 0;
	int64 Frame = 0, Wait = 0;
};
