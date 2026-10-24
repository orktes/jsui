// --selftest: the C++ port of SelfTest in integrations/shared/example_host.h
// (and the Unity example's SelfTest.cs). Once the UI has reported where its
// Spawn button is, it clicks like the OS does: FSlateApplication's platform
// message handler (OnMouseMove / OnMouseDown / OnMouseUp) with the window and
// desktop coordinates taken from the native window, not from Slate's idea of
// where the window is. So the clicks take the same path as real ones (input
// preprocessors, SJsuiView's mapping to UI px, Slate routing). It checks the
// pass-through contract:
//   1. a click on empty space (the middle of the screen) is not consumed:
//      jsui_hit_test says nothing is there and the game sees the click;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the game, which adds a unit.
// Logs "selftest: ok" (exit 0) or what failed (exit 1).
#pragma once

#include "CoreMinimal.h"

class SJsuiView;

class FJsuiSelfTest
{
public:
	// The number after "key": in a flat JSON object (messages of app.jsx are
	// tiny and fixed; a real game would use its JSON library).
	static double JsonNumber(const FString& Json, const TCHAR* Key, double Fallback = 0);

	void OnMessage(const FString& Name, const FString& Json);
	// Once per frame. Entities: units in the game; Clicks: presses the UI
	// passed on to the game. Returns 0 while running, 1 when passed, -1 when
	// failed.
	int32 Step(SJsuiView& View, int32 Entities, int32 Clicks);

private:
	static int32 Fail(const TCHAR* What);
	// A left click at UI coordinates, through FSlateApplication as the OS
	// sends it. False when the view is not in a window yet.
	static bool Click(SJsuiView& View, const FVector2D& Ui);

	bool bAnchors = false;
	float SpawnX = 0, SpawnY = 0;
	int32 Phase = 0, Before = 0, ClicksBefore = 0;
	int64 Frame = 0, Wait = 0;
};
