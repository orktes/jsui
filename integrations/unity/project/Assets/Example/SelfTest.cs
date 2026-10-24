// --selftest: the C# port of SelfTest in integrations/shared/example_host.h.
// Once the UI has reported where its Spawn button is, it sends synthetic
// input straight to the runtime and checks the pass-through contract:
//   1. a click on empty space (the middle of the screen) is not consumed,
//      and jsui_hit_test says nothing is there;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the game, which adds a unit.
// Logs "selftest: ok" (exit 0) or what failed (exit 1).
using System;
using System.Globalization;
using Jsui;
using UnityEngine;

public class SelfTest
{
    bool anchors_;
    float spawnX_, spawnY_;
    int phase_, before_;
    long frame_, wait_;

    // The number after "key": in a flat JSON object (messages of app.jsx are
    // tiny and fixed; a real game would use its JSON library).
    public static double JsonNumber(string json, string key, double fallback = 0)
    {
        if (json == null) return fallback;
        var k = "\"" + key + "\":";
        int i = json.IndexOf(k, StringComparison.Ordinal);
        if (i < 0) return fallback;
        i += k.Length;
        int j = i;
        while (j < json.Length && "+-.0123456789eE".IndexOf(json[j]) >= 0) j++;
        return double.TryParse(json.Substring(i, j - i), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;
    }

    public void OnMessage(string name, string json)
    {
        if (name != "ui:anchors") return;
        spawnX_ = (float)(JsonNumber(json, "x") + JsonNumber(json, "width") / 2);
        spawnY_ = (float)(JsonNumber(json, "y") + JsonNumber(json, "height") / 2);
        anchors_ = true;
    }

    // Once per frame after jsui_tick. w, h: viewport (logical px).
    // Returns 0 while running, 1 when passed, -1 when failed.
    public int Step(IntPtr rt, float w, float h, int entities)
    {
        frame_++;
        switch (phase_)
        {
            case 0:  // wait for the UI and a few frames of layout
                if (anchors_ && frame_ > 30) phase_ = 1;
                else if (frame_ > 900) return Fail("the UI never reported ui:anchors");
                return 0;
            case 1:
            {
                float x = w * 0.5f, y = h * 0.45f;
                if (JsuiNative.jsui_hit_test(rt, x, y) != 0) return Fail("jsui_hit_test reports UI over the empty middle of the screen");
                JsuiNative.jsui_send_mouse_move(rt, x, y);
                int down = JsuiNative.jsui_send_mouse_button(rt, x, y, JsuiNative.MouseLeft, 1, 0);
                int up = JsuiNative.jsui_send_mouse_button(rt, x, y, JsuiNative.MouseLeft, 0, 0);
                if (down != 0 || up != 0) return Fail("a click on empty space was consumed");
                Debug.Log("selftest: empty-space click passed through to the game");
                phase_ = 2;
                return 0;
            }
            case 2:
            {
                if ((JsuiNative.jsui_hit_test(rt, spawnX_, spawnY_) & JsuiNative.HitPointer) == 0)
                    return Fail("jsui_hit_test misses the Spawn button");
                before_ = entities;
                JsuiNative.jsui_send_mouse_move(rt, spawnX_, spawnY_);
                int down = JsuiNative.jsui_send_mouse_button(rt, spawnX_, spawnY_, JsuiNative.MouseLeft, 1, 0);
                int up = JsuiNative.jsui_send_mouse_button(rt, spawnX_, spawnY_, JsuiNative.MouseLeft, 0, 0);
                if (down == 0 || up == 0) return Fail("the click on the Spawn button was not consumed");
                JsuiNative.jsui_send_mouse_leave(rt);
                Debug.Log("selftest: Spawn button click consumed by the UI");
                phase_ = 3;
                wait_ = frame_;
                return 0;
            }
            case 3:
                if (entities == before_ + 1)
                {
                    Debug.Log($"selftest: game:spawn reached the game ({before_} -> {entities} units)");
                    phase_ = 4;
                    wait_ = frame_;
                    return 0;
                }
                if (frame_ - wait_ > 120) return Fail("game:spawn never arrived");
                return 0;
            default:
                // A few more frames so the new unit's nameplate is on screen
                // for --screenshot.
                if (frame_ - wait_ < 20) return 0;
                Debug.Log("selftest: ok");
                return 1;
        }
    }

    static int Fail(string what)
    {
        Debug.LogError("selftest: FAILED: " + what);
        return -1;
    }
}
