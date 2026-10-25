// --selftest: the C# port of SelfTest in integrations/shared/example_host.h.
// Once the UI has reported where its Spawn button is, it stalls JS for a
// moment, then clicks through JsuiView.HandlePointer, the entry point Unity's
// Input goes through each frame (IHost.Click), so the clicks take the view's
// own path: mapping to UI px, jsui_send_*, the Passed* events and the game's
// handlers. What it skips is Unity's Input Manager itself (OS events ->
// Input.mousePosition / GetMouseButtonDown), which has no way to inject
// input. It checks:
//   0. with JS on its own thread (JsuiView.jsThread On, the default), a JS
//      stall (jsui_debug_stall_js) holds no game frame: the frames go on
//      meanwhile and JS catches up after it;
// and the pass-through contract:
//   1. a click on empty space (the middle of the screen) is not consumed:
//      jsui_hit_test says nothing is there and the game counts the click;
//   2. a click on the Spawn button is consumed, and its game:spawn message
//      reaches the game, which adds a unit;
//   3. a click on a unit selects it, and the UI reports the selection back
//      (ui:selected); a click on empty ground clears it; another click
//      selects again (so --screenshot shows a selected unit).
// Logs "selftest: ok" (exit 0) or what failed (exit 1).
using System;
using System.Globalization;
using Jsui;
using UnityEngine;

public class SelfTest
{
    // What the test drives in the game.
    public interface IHost
    {
        int Entities { get; }
        int Selected { get; }
        int Clicks { get; }  // presses the UI passed on to the game
        // A left click at `ui` (logical px) through the game's input path.
        void Click(Vector2 ui);
        // The unit a click at `ui` selects, or -1.
        int Pick(Vector2 ui);
        // Unit `i`'s centre on screen (logical px); false when off screen.
        bool UnitOnScreen(int i, out Vector2 ui);
    }

    bool anchors_;
    float spawnX_, spawnY_;
    int phase_, before_, round_, expect_ = -1;
    int reported_ = -1, reports_, reportsBefore_;
    long frame_, wait_;
    // The stall phase.
    const int kStall = 100;
    const double kStallMs = 300;
    int stallFrames_;
    double stallStart_, lastFrame_, maxGap_;
    static double NowMs() => System.Diagnostics.Stopwatch.GetTimestamp() * 1000.0 / System.Diagnostics.Stopwatch.Frequency;

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
        if (name == "ui:anchors")
        {
            spawnX_ = (float)(JsonNumber(json, "x") + JsonNumber(json, "width") / 2);
            spawnY_ = (float)(JsonNumber(json, "y") + JsonNumber(json, "height") / 2);
            anchors_ = true;
        }
        else if (name == "ui:selected")
        {
            reported_ = (int)JsonNumber(json, "index", -1);
            reports_++;
        }
    }

    // Once per frame after jsui_tick. w, h: viewport (logical px).
    // Returns 0 while running, 1 when passed, -1 when failed.
    public int Step(IntPtr rt, IHost host, float w, float h)
    {
        frame_++;
        switch (phase_)
        {
            case 0:  // wait for the UI and a few frames of layout
                if (anchors_ && frame_ > 30) phase_ = kStall;
                else if (frame_ > 900) return Fail("the UI never reported ui:anchors");
                return 0;
            case kStall:
            {
                int r = StallStep(rt);
                if (r > 0) phase_ = 1;
                return r < 0 ? -1 : 0;
            }
            case 1:
            {
                var p = new Vector2(w * 0.5f, h * 0.45f);
                if (JsuiNative.jsui_hit_test(rt, p.x, p.y) != 0) return Fail("jsui_hit_test reports UI over the empty middle of the screen");
                int clicks = host.Clicks;
                host.Click(p);
                if (host.Clicks != clicks + 1) return Fail("a click on empty space did not reach the game");
                Debug.Log("selftest: empty-space click passed through to the game");
                phase_ = 2;
                return 0;
            }
            case 2:
            {
                if ((JsuiNative.jsui_hit_test(rt, spawnX_, spawnY_) & JsuiNative.HitPointer) == 0)
                    return Fail("jsui_hit_test misses the Spawn button");
                before_ = host.Entities;
                int clicks = host.Clicks;
                host.Click(new Vector2(spawnX_, spawnY_));
                if (host.Clicks != clicks) return Fail("the click on the Spawn button was passed to the game");
                Debug.Log("selftest: Spawn button click consumed by the UI");
                phase_ = 3;
                wait_ = frame_;
                return 0;
            }
            case 3:
                if (host.Entities == before_ + 1)
                {
                    Debug.Log($"selftest: game:spawn reached the game ({before_} -> {host.Entities} units)");
                    phase_ = 4;
                    return 0;
                }
                if (frame_ - wait_ > 120) return Fail("game:spawn never arrived");
                return 0;
            case 4:  // select a unit, clear, select again
            {
                Vector2 p;
                int want = -1;
                if (round_ == 1)
                {
                    if (!EmptyGround(rt, host, w, h, out p)) return Fail("no empty ground to click");
                }
                else
                {
                    want = ClickableUnit(rt, host, w, h, out p);
                    if (want < 0) return Fail("no unit on screen to click");
                }
                host.Click(p);
                if (host.Selected != want)
                {
                    Debug.LogError($"selftest: clicked ({p.x:0}, {p.y:0}): selected {host.Selected}, expected {want}");
                    return Fail(want < 0 ? "a click on empty ground did not clear the selection" : "a click on a unit did not select it");
                }
                expect_ = want;
                reportsBefore_ = reports_;
                phase_ = 5;
                wait_ = frame_;
                return 0;
            }
            case 5:
                if (reports_ > reportsBefore_ && reported_ == expect_)
                {
                    Debug.Log(expect_ >= 0
                        ? $"selftest: a click on unit {expect_} selected it; the UI reports it"
                        : "selftest: a click on empty ground cleared the selection; the UI reports it");
                    if (++round_ < 3)
                    {
                        phase_ = 4;
                    }
                    else
                    {
                        phase_ = 6;
                        wait_ = frame_;
                    }
                    return 0;
                }
                if (frame_ - wait_ > 120) return Fail("the UI never reported the selection (ui:selected)");
                return 0;
            default:
                // A few more frames so the selection is on screen for
                // --screenshot.
                if (frame_ - wait_ < 20) return 0;
                Debug.Log("selftest: ok");
                return 1;
        }
    }

    // One JS stall of kStallMs, stepped once per game frame: 1 when done,
    // -1 when it failed. With a JS thread, posting it returns at once, no gap
    // between two game frames comes near the stall while it runs, and
    // jsui_sync sees JS idle again after it. Off or in lockstep the post
    // itself holds the game for the stall.
    int StallStep(IntPtr rt)
    {
        double now = NowMs();
        if (stallFrames_ == 0)
        {
            int mode = JsuiNative.jsui_js_thread(rt);
            JsuiNative.jsui_debug_stall_js(rt, kStallMs);
            double held = NowMs() - now;
            if (mode != JsuiNative.JsThreadOn)
            {
                if (held < kStallMs * 0.9) return Fail("jsui_debug_stall_js did not keep JS busy");
                Debug.Log($"selftest: JS thread {(JsuiView.JsThreadMode)mode}: a {kStallMs:0} ms JS stall held the game for {held:0} ms");
                return 1;
            }
            if (held > kStallMs / 4) return Fail("posting a JS stall waited for the JS thread");
            stallStart_ = lastFrame_ = NowMs();
            stallFrames_ = 1;
            return 0;
        }
        double gap = now - lastFrame_;
        lastFrame_ = now;
        maxGap_ = Math.Max(maxGap_, gap);
        if (now - stallStart_ < kStallMs)
        {
            if (gap > kStallMs / 2) return Fail("a game frame waited for the busy JS thread");
            stallFrames_++;
            return 0;
        }
        if (stallFrames_ < 3) return Fail("too few game frames while JS was busy");
        if (JsuiNative.jsui_sync(rt, 2000) != 1) return Fail("JS did not come back from its stall");
        Debug.Log($"selftest: JS thread On: {stallFrames_} game frames during a {kStallMs:0} ms JS stall, the longest {maxGap_:0.0} ms");
        return 1;
    }

    // A unit whose centre is on screen, away from the edges, not under the
    // UI, and picked there (not hidden behind another one). The newest first.
    static int ClickableUnit(IntPtr rt, IHost host, float w, float h, out Vector2 p)
    {
        for (int i = host.Entities - 1; i >= 0; i--)
        {
            if (!host.UnitOnScreen(i, out p)) continue;
            if (p.x < 40 || p.y < 40 || p.x > w - 40 || p.y > h - 40) continue;
            if (JsuiNative.jsui_hit_test(rt, p.x, p.y) != 0) continue;
            if (host.Pick(p) == i) return i;
        }
        p = Vector2.zero;
        return -1;
    }

    // A point with neither UI nor a unit under it.
    static bool EmptyGround(IntPtr rt, IHost host, float w, float h, out Vector2 p)
    {
        for (int gy = 2; gy < 10; gy++)
            for (int gx = 1; gx < 16; gx++)
            {
                p = new Vector2(w * gx / 16f, h * gy / 12f);
                if (JsuiNative.jsui_hit_test(rt, p.x, p.y) == 0 && host.Pick(p) < 0) return true;
            }
        p = Vector2.zero;
        return false;
    }

    static int Fail(string what)
    {
        Debug.LogError("selftest: FAILED: " + what);
        return -1;
    }
}
