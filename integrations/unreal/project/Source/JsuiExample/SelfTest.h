// --selftest: the C++ port of SelfTest in integrations/shared/example_host.h
// (and the Unity example's SelfTest.cs). Once the UI has reported where its
// Spawn button is, it sends synthetic input straight to the runtime and checks
// the pass-through contract:
//   1. a click on empty space (the middle of the screen) is not consumed, and
//      jsui_hit_test says nothing is there;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the game, which adds a unit.
// Logs "selftest: ok" (exit 0) or what failed (exit 1).
#pragma once

#include "CoreMinimal.h"

struct JsuiRuntime;

class FJsuiSelfTest
{
public:
	// The number after "key": in a flat JSON object (messages of app.jsx are
	// tiny and fixed; a real game would use its JSON library).
	static double JsonNumber(const FString& Json, const TCHAR* Key, double Fallback = 0);

	void OnMessage(const FString& Name, const FString& Json);
	// Once per frame. W, H: viewport (logical px). Returns 0 while running,
	// 1 when passed, -1 when failed.
	int32 Step(JsuiRuntime* Rt, float W, float H, int32 Entities);

private:
	static int32 Fail(const TCHAR* What);

	bool bAnchors = false;
	float SpawnX = 0, SpawnY = 0;
	int32 Phase = 0, Before = 0;
	int64 Frame = 0, Wait = 0;
};
