// Unity KeyCode -> the DOM KeyboardEvent names jsui_send_key takes
// (`code`: the physical key, "KeyA"; `key`: the logical key, "a" / "Enter").
// Printable keys follow the US layout for shifted symbols; the typed text
// itself is sent separately (jsui_send_text), as browsers do.
using UnityEngine;

namespace Jsui
{
    public static class JsuiKeys
    {
        public static string DomCode(KeyCode k)
        {
            if (k >= KeyCode.A && k <= KeyCode.Z) return "Key" + (char)('A' + (k - KeyCode.A));
            if (k >= KeyCode.Alpha0 && k <= KeyCode.Alpha9) return "Digit" + (k - KeyCode.Alpha0);
            if (k >= KeyCode.Keypad0 && k <= KeyCode.Keypad9) return "Numpad" + (k - KeyCode.Keypad0);
            if (k >= KeyCode.F1 && k <= KeyCode.F15) return "F" + (1 + (k - KeyCode.F1));
            switch (k)
            {
                case KeyCode.Space: return "Space";
                case KeyCode.Quote: return "Quote";
                case KeyCode.Comma: return "Comma";
                case KeyCode.Minus: return "Minus";
                case KeyCode.Period: return "Period";
                case KeyCode.Slash: return "Slash";
                case KeyCode.Semicolon: return "Semicolon";
                case KeyCode.Equals: return "Equal";
                case KeyCode.LeftBracket: return "BracketLeft";
                case KeyCode.Backslash: return "Backslash";
                case KeyCode.RightBracket: return "BracketRight";
                case KeyCode.BackQuote: return "Backquote";
                case KeyCode.Escape: return "Escape";
                case KeyCode.Return: return "Enter";
                case KeyCode.Tab: return "Tab";
                case KeyCode.Backspace: return "Backspace";
                case KeyCode.Insert: return "Insert";
                case KeyCode.Delete: return "Delete";
                case KeyCode.RightArrow: return "ArrowRight";
                case KeyCode.LeftArrow: return "ArrowLeft";
                case KeyCode.DownArrow: return "ArrowDown";
                case KeyCode.UpArrow: return "ArrowUp";
                case KeyCode.PageUp: return "PageUp";
                case KeyCode.PageDown: return "PageDown";
                case KeyCode.Home: return "Home";
                case KeyCode.End: return "End";
                case KeyCode.CapsLock: return "CapsLock";
                case KeyCode.ScrollLock: return "ScrollLock";
                case KeyCode.Numlock: return "NumLock";
                case KeyCode.Print: return "PrintScreen";
                case KeyCode.Pause: return "Pause";
                case KeyCode.KeypadPeriod: return "NumpadDecimal";
                case KeyCode.KeypadDivide: return "NumpadDivide";
                case KeyCode.KeypadMultiply: return "NumpadMultiply";
                case KeyCode.KeypadMinus: return "NumpadSubtract";
                case KeyCode.KeypadPlus: return "NumpadAdd";
                case KeyCode.KeypadEnter: return "NumpadEnter";
                case KeyCode.KeypadEquals: return "NumpadEqual";
                case KeyCode.LeftShift: return "ShiftLeft";
                case KeyCode.RightShift: return "ShiftRight";
                case KeyCode.LeftControl: return "ControlLeft";
                case KeyCode.RightControl: return "ControlRight";
                case KeyCode.LeftAlt: return "AltLeft";
                case KeyCode.RightAlt: return "AltRight";
                case KeyCode.LeftCommand: return "MetaLeft";
                case KeyCode.RightCommand: return "MetaRight";
                case KeyCode.LeftWindows: return "MetaLeft";
                case KeyCode.RightWindows: return "MetaRight";
                case KeyCode.Menu: return "ContextMenu";
                default: return null;
            }
        }

        const string kPlain = " ',-./0123456789;=[\\]`";
        const string kShift = " \"<_>?)!@#$%^&*(:+{|}~";

        static int PrintableIndex(KeyCode k)
        {
            switch (k)
            {
                case KeyCode.Space: return 0;
                case KeyCode.Quote: return 1;
                case KeyCode.Comma: return 2;
                case KeyCode.Minus: return 3;
                case KeyCode.Period: return 4;
                case KeyCode.Slash: return 5;
                case KeyCode.Semicolon: return 16;
                case KeyCode.Equals: return 17;
                case KeyCode.LeftBracket: return 18;
                case KeyCode.Backslash: return 19;
                case KeyCode.RightBracket: return 20;
                case KeyCode.BackQuote: return 21;
                default:
                    if (k >= KeyCode.Alpha0 && k <= KeyCode.Alpha9) return 6 + (k - KeyCode.Alpha0);
                    return -1;
            }
        }

        public static string DomKey(KeyCode k, bool shift)
        {
            if (k >= KeyCode.A && k <= KeyCode.Z)
                return ((char)((shift ? 'A' : 'a') + (k - KeyCode.A))).ToString();
            int i = PrintableIndex(k);
            if (i >= 0) return (shift ? kShift[i] : kPlain[i]).ToString();
            if (k >= KeyCode.Keypad0 && k <= KeyCode.Keypad9) return ((char)('0' + (k - KeyCode.Keypad0))).ToString();
            if (k >= KeyCode.F1 && k <= KeyCode.F15) return "F" + (1 + (k - KeyCode.F1));
            switch (k)
            {
                case KeyCode.Return:
                case KeyCode.KeypadEnter: return "Enter";
                case KeyCode.KeypadPeriod: return ".";
                case KeyCode.KeypadDivide: return "/";
                case KeyCode.KeypadMultiply: return "*";
                case KeyCode.KeypadMinus: return "-";
                case KeyCode.KeypadPlus: return "+";
                case KeyCode.KeypadEquals: return "=";
                case KeyCode.LeftShift:
                case KeyCode.RightShift: return "Shift";
                case KeyCode.LeftControl:
                case KeyCode.RightControl: return "Control";
                case KeyCode.LeftAlt:
                case KeyCode.RightAlt: return "Alt";
                case KeyCode.LeftCommand:
                case KeyCode.RightCommand:
                case KeyCode.LeftWindows:
                case KeyCode.RightWindows: return "Meta";
                case KeyCode.Menu: return "ContextMenu";
                case KeyCode.Numlock: return "NumLock";
                case KeyCode.Print: return "PrintScreen";
            }
            var code = DomCode(k);
            return code ?? "Unidentified";  // the rest name the key the same as its code
        }
    }
}
