// Unreal FKey -> the DOM KeyboardEvent names jsui_send_key takes (`code`: the
// physical key, "KeyA"; `key`: the logical key, "a" / "Enter"). Printable
// keys follow the US layout for shifted symbols; the typed text itself is
// sent separately (jsui_send_text, from SJsuiView::OnKeyChar), as browsers do.
#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

namespace JsuiKeys
{
struct FEntry
{
	const TCHAR* Unreal;  // FKey name
	const TCHAR* Code;
	const TCHAR* Key;
	const TCHAR* Shifted;  // `key` with Shift, or nullptr: same
};

inline const FEntry* Find(const FKey& K)
{
	static const FEntry Table[] = {
		{TEXT("SpaceBar"), TEXT("Space"), TEXT(" "), nullptr},
		{TEXT("Enter"), TEXT("Enter"), TEXT("Enter"), nullptr},
		{TEXT("Escape"), TEXT("Escape"), TEXT("Escape"), nullptr},
		{TEXT("Tab"), TEXT("Tab"), TEXT("Tab"), nullptr},
		{TEXT("BackSpace"), TEXT("Backspace"), TEXT("Backspace"), nullptr},
		{TEXT("Delete"), TEXT("Delete"), TEXT("Delete"), nullptr},
		{TEXT("Insert"), TEXT("Insert"), TEXT("Insert"), nullptr},
		{TEXT("Home"), TEXT("Home"), TEXT("Home"), nullptr},
		{TEXT("End"), TEXT("End"), TEXT("End"), nullptr},
		{TEXT("PageUp"), TEXT("PageUp"), TEXT("PageUp"), nullptr},
		{TEXT("PageDown"), TEXT("PageDown"), TEXT("PageDown"), nullptr},
		{TEXT("Left"), TEXT("ArrowLeft"), TEXT("ArrowLeft"), nullptr},
		{TEXT("Right"), TEXT("ArrowRight"), TEXT("ArrowRight"), nullptr},
		{TEXT("Up"), TEXT("ArrowUp"), TEXT("ArrowUp"), nullptr},
		{TEXT("Down"), TEXT("ArrowDown"), TEXT("ArrowDown"), nullptr},
		{TEXT("LeftShift"), TEXT("ShiftLeft"), TEXT("Shift"), nullptr},
		{TEXT("RightShift"), TEXT("ShiftRight"), TEXT("Shift"), nullptr},
		{TEXT("LeftControl"), TEXT("ControlLeft"), TEXT("Control"), nullptr},
		{TEXT("RightControl"), TEXT("ControlRight"), TEXT("Control"), nullptr},
		{TEXT("LeftAlt"), TEXT("AltLeft"), TEXT("Alt"), nullptr},
		{TEXT("RightAlt"), TEXT("AltRight"), TEXT("Alt"), nullptr},
		{TEXT("LeftCommand"), TEXT("MetaLeft"), TEXT("Meta"), nullptr},
		{TEXT("RightCommand"), TEXT("MetaRight"), TEXT("Meta"), nullptr},
		{TEXT("CapsLock"), TEXT("CapsLock"), TEXT("CapsLock"), nullptr},
		{TEXT("NumLock"), TEXT("NumLock"), TEXT("NumLock"), nullptr},
		{TEXT("ScrollLock"), TEXT("ScrollLock"), TEXT("ScrollLock"), nullptr},
		{TEXT("Pause"), TEXT("Pause"), TEXT("Pause"), nullptr},
		{TEXT("Semicolon"), TEXT("Semicolon"), TEXT(";"), TEXT(":")},
		{TEXT("Equals"), TEXT("Equal"), TEXT("="), TEXT("+")},
		{TEXT("Comma"), TEXT("Comma"), TEXT(","), TEXT("<")},
		{TEXT("Hyphen"), TEXT("Minus"), TEXT("-"), TEXT("_")},
		{TEXT("Period"), TEXT("Period"), TEXT("."), TEXT(">")},
		{TEXT("Slash"), TEXT("Slash"), TEXT("/"), TEXT("?")},
		{TEXT("Tilde"), TEXT("Backquote"), TEXT("`"), TEXT("~")},
		{TEXT("LeftBracket"), TEXT("BracketLeft"), TEXT("["), TEXT("{")},
		{TEXT("Backslash"), TEXT("Backslash"), TEXT("\\"), TEXT("|")},
		{TEXT("RightBracket"), TEXT("BracketRight"), TEXT("]"), TEXT("}")},
		{TEXT("Apostrophe"), TEXT("Quote"), TEXT("'"), TEXT("\"")},
		{TEXT("Multiply"), TEXT("NumpadMultiply"), TEXT("*"), nullptr},
		{TEXT("Add"), TEXT("NumpadAdd"), TEXT("+"), nullptr},
		{TEXT("Subtract"), TEXT("NumpadSubtract"), TEXT("-"), nullptr},
		{TEXT("Decimal"), TEXT("NumpadDecimal"), TEXT("."), nullptr},
		{TEXT("Divide"), TEXT("NumpadDivide"), TEXT("/"), nullptr},
	};
	const FName Name = K.GetFName();
	for (const FEntry& E : Table)
	{
		if (Name == FName(E.Unreal)) return &E;
	}
	return nullptr;
}

inline int32 Digit(const FString& Name)
{
	static const TCHAR* Names[] = {TEXT("Zero"), TEXT("One"), TEXT("Two"), TEXT("Three"), TEXT("Four"),
	                               TEXT("Five"), TEXT("Six"), TEXT("Seven"), TEXT("Eight"), TEXT("Nine")};
	for (int32 I = 0; I < 10; I++)
	{
		if (Name == Names[I]) return I;
	}
	return -1;
}

inline FString DomCode(const FKey& K)
{
	const FString Name = K.GetFName().ToString();
	if (Name.Len() == 1 && FChar::IsAlpha(Name[0])) return TEXT("Key") + Name.ToUpper();
	if (int32 D = Digit(Name); D >= 0) return FString::Printf(TEXT("Digit%d"), D);
	if (Name.StartsWith(TEXT("NumPad")))
	{
		if (int32 D = Digit(Name.Mid(6)); D >= 0) return FString::Printf(TEXT("Numpad%d"), D);
	}
	if (Name.Len() >= 2 && Name[0] == 'F' && FChar::IsDigit(Name[1])) return Name;  // F1..F24
	if (const FEntry* E = Find(K)) return E->Code;
	return TEXT("Unidentified");
}

inline FString DomKey(const FKey& K, bool bShift)
{
	static const TCHAR* ShiftedDigits = TEXT(")!@#$%^&*(");
	const FString Name = K.GetFName().ToString();
	if (Name.Len() == 1 && FChar::IsAlpha(Name[0])) return bShift ? Name.ToUpper() : Name.ToLower();
	if (int32 D = Digit(Name); D >= 0) return bShift ? FString(1, &ShiftedDigits[D]) : FString::FromInt(D);
	if (Name.StartsWith(TEXT("NumPad")))
	{
		if (int32 D = Digit(Name.Mid(6)); D >= 0) return FString::FromInt(D);
	}
	if (Name.Len() >= 2 && Name[0] == 'F' && FChar::IsDigit(Name[1])) return Name;
	if (const FEntry* E = Find(K)) return (bShift && E->Shifted) ? E->Shifted : E->Key;
	return TEXT("Unidentified");
}
}  // namespace JsuiKeys
