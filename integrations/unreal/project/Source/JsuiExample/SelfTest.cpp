#include "SelfTest.h"

#include "JsuiExampleLog.h"

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

int32 FJsuiSelfTest::Step(JsuiRuntime* Rt, float W, float H, int32 Entities)
{
	Frame++;
	switch (Phase)
	{
	case 0:  // wait for the UI and a few frames of layout
		if (bAnchors && Frame > 30) Phase = 1;
		else if (Frame > 900) return Fail(TEXT("the UI never reported ui:anchors"));
		return 0;
	case 1:
	{
		const float X = W * 0.5f, Y = H * 0.45f;
		if (jsui_hit_test(Rt, X, Y) != 0) return Fail(TEXT("jsui_hit_test reports UI over the empty middle of the screen"));
		jsui_send_mouse_move(Rt, X, Y);
		const int Down = jsui_send_mouse_button(Rt, X, Y, JSUI_MOUSE_LEFT, 1, 0);
		const int Up = jsui_send_mouse_button(Rt, X, Y, JSUI_MOUSE_LEFT, 0, 0);
		if (Down != 0 || Up != 0) return Fail(TEXT("a click on empty space was consumed"));
		JsuiExampleSay(TEXT("selftest: empty-space click passed through to the game"));
		Phase = 2;
		return 0;
	}
	case 2:
	{
		if ((jsui_hit_test(Rt, SpawnX, SpawnY) & JSUI_HIT_POINTER) == 0) return Fail(TEXT("jsui_hit_test misses the Spawn button"));
		Before = Entities;
		jsui_send_mouse_move(Rt, SpawnX, SpawnY);
		const int Down = jsui_send_mouse_button(Rt, SpawnX, SpawnY, JSUI_MOUSE_LEFT, 1, 0);
		const int Up = jsui_send_mouse_button(Rt, SpawnX, SpawnY, JSUI_MOUSE_LEFT, 0, 0);
		if (Down == 0 || Up == 0) return Fail(TEXT("the click on the Spawn button was not consumed"));
		jsui_send_mouse_leave(Rt);
		JsuiExampleSay(TEXT("selftest: Spawn button click consumed by the UI"));
		Phase = 3;
		Wait = Frame;
		return 0;
	}
	case 3:
		if (Entities == Before + 1)
		{
			JsuiExampleSay(*FString::Printf(TEXT("selftest: game:spawn reached the game (%d -> %d units)"), Before, Entities));
			Phase = 4;
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
