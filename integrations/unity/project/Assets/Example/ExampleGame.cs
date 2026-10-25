// The Unity version of the engine examples' game (integrations/raylib,
// sdl3, sokol): a small 3D scene with the shared HUD
// (integrations/shared/ui/app.jsx) on top, through JsuiView.
//
// The scene is built in code, so the example needs no scene asset: the
// build script (Assets/Editor/BuildExample.cs) saves an empty scene with
// this component on one GameObject.
//
// Input pass-through: JsuiView raises PassedMouseDown / PassedMouseUp /
// PassedWheel / PassedKeyDown only for input the UI did not consume. Click a
// unit to select it (a ray from the camera against each cube) or empty
// ground to clear the selection, drag empty space to orbit the camera, wheel
// to zoom, Space spawns a unit, Esc pauses.
//
// Per-frame data: every unit is projected to the screen each frame and
// written as x, y, visible, hue into the `entities` frame buffer, which moves
// the nameplates without running JS; fps, unit count, the selected unit and
// the pass-through counters go to the `game` shared state.
//
// Command line (players): --selftest, --frames N, --screenshot out.png,
// --bundle path-or-url, --dev, --cpu (the CPU renderer even where the GPU path works),
// --js-thread on|off|lockstep (JsuiView.jsThread; on by default),
// --frame-stats (log a histogram of frame times every 10 s).
//
// Frame pacing: interactive runs sync to the display (vSyncCount 1, no
// targetFrameRate), as a game would; --selftest and --frames run uncapped.
// On macOS, Unity 6000.3's player paces vSync at a whole 59 Hz on a 59.9x Hz
// display (--frame-stats: a steady 16.95 ms, no missed frames), so the HUD
// shows 59 there; no Unity setting changes that (targetFrameRate,
// maxQueuedFrames, Screen.SetResolution's refresh rate, fullscreen).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using Jsui;
using UnityEngine;
using Random = UnityEngine.Random;

public class ExampleGame : MonoBehaviour, SelfTest.IHost
{
    [Tooltip("Lit material the scene's materials are copied from (set by the build script, so players include its shader)")]
    public Material baseMaterial;

    class Unit
    {
        public float angle, radius, speed, height, size, phase;
        public int hue;
        public Transform body;
    }

    static readonly Color32[] kHues =
    {
        new Color32(242, 195, 91, 255), new Color32(79, 209, 255, 255), new Color32(178, 107, 255, 255),
        new Color32(74, 222, 128, 255), new Color32(255, 154, 60, 255), new Color32(79, 140, 255, 255),
        new Color32(255, 77, 106, 255),
    };

    JsuiView view_;
    Camera camera_;
    readonly List<Unit> units_ = new List<Unit>();
    readonly float[] entities_ = new float[48 * 4];
    Material[] materials_;
    int spawned_;
    float time_, yaw_ = 0.8f, pitch_ = 0.55f, distance_ = 16f;
    bool paused_, dragging_;
    Vector2 pressAt_, lastPointer_;  // UI px
    float travel_;                    // how far the pointer went since the press
    const float kClickSlop = 4f;      // a press that moved less is a click
    int selected_ = -1;
    Material selectedMaterial_, ringMaterial_;
    Transform ring_;
    int clicks_, wheel_, keys_;
    // FPS over the last 60 frames after a 10-frame warm-up, shown at most
    // every 250 ms (FpsCounter in integrations/shared/example_host.h).
    const int kFpsWindow = 60, kFpsWarmup = 10;
    readonly double[] frameTimes_ = new double[60];
    long framesSeen_;
    double fpsShownAt_ = -1;
    int fps_;
    string lastState_;

    // Command line.
    bool selftest_;
    long frames_ = -1, frame_;
    string screenshot_;
    SelfTest test_;

    void Awake()
    {
        ParseArgs();
        if (frames_ >= 0 || selftest_)
        {
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = -1;
        }
        else
        {
            // Every display refresh, paced by the swap chain.
            QualitySettings.vSyncCount = 1;
            Application.targetFrameRate = -1;
        }
        if (frameStats_)
            Debug.Log($"frame-stats: vSyncCount {QualitySettings.vSyncCount}, targetFrameRate {Application.targetFrameRate}, " +
                      $"refresh {Screen.currentResolution.refreshRateRatio.value:0.##} Hz");
        BuildScene();

        view_ = gameObject.AddComponent<JsuiView>();
        view_.forwardInput = !selftest_;
        view_.forceCpu = cpu_;
        if (bundleOverride_ != null) view_.bundle = bundleOverride_;
        view_.devMode = dev_;
        view_.jsThread = jsThread_;  // read in the view's Start
        view_.OnMessage += OnUiMessage;
        view_.PassedMouseDown += (button, p) =>
        {
            if (button != 0) return;
            dragging_ = true;  // the press is the game's: orbit while held, select if it was a click
            pressAt_ = lastPointer_ = p;
            travel_ = 0;
            clicks_++;
        };
        view_.PassedMouseUp += (button, p) =>
        {
            if (button != 0 || !dragging_) return;
            dragging_ = false;
            travel_ = Mathf.Max(travel_, Vector2.Distance(p, pressAt_));
            if (travel_ <= kClickSlop) Select(Pick(p));
        };
        view_.PassedWheel += w =>
        {
            distance_ = Mathf.Clamp(distance_ * (w.y > 0 ? 0.9f : 1.1f), 6f, 40f);
            wheel_++;
        };
        view_.PassedKeyDown += k =>
        {
            if (k == KeyCode.Space) Spawn();
            else if (k == KeyCode.Escape) paused_ = !paused_;
            else return;
            keys_++;
        };
        if (selftest_) test_ = new SelfTest();
        for (int i = 0; i < 8; i++) Spawn();
        PublishState();  // before the bundle runs (JsuiView.Start): its first render sees it
    }

    string bundleOverride_;
    bool dev_, cpu_, frameStats_;
    JsuiView.JsThreadMode jsThread_ = JsuiView.JsThreadMode.On;

    void ParseArgs()
    {
        var args = Environment.GetCommandLineArgs();
        for (int i = 1; i < args.Length; i++)
        {
            string a = args[i];
            string Next() => i + 1 < args.Length ? args[++i] : "";
            if (a == "--selftest") selftest_ = true;
            else if (a == "--frames") frames_ = long.Parse(Next(), CultureInfo.InvariantCulture);
            else if (a == "--screenshot") screenshot_ = Next();
            else if (a == "--bundle") bundleOverride_ = Next();
            else if (a == "--dev") dev_ = true;
            else if (a == "--cpu") cpu_ = true;
            else if (a == "--frame-stats") frameStats_ = true;
            else if (a == "--js-thread")
            {
                var m = Next();
                jsThread_ = m == "off" ? JsuiView.JsThreadMode.Off
                    : m == "lockstep" ? JsuiView.JsThreadMode.Lockstep
                    : JsuiView.JsThreadMode.On;
            }
        }
        if (!string.IsNullOrEmpty(screenshot_) && frames_ < 0 && !selftest_) frames_ = 120;
    }

    // ------------------------------------------------------------ scene

    void BuildScene()
    {
        camera_ = new GameObject("Camera").AddComponent<Camera>();
        camera_.clearFlags = CameraClearFlags.SolidColor;
        camera_.backgroundColor = new Color32(9, 12, 22, 255);
        camera_.fieldOfView = 50;
        camera_.allowMSAA = true;

        var light = new GameObject("Light").AddComponent<Light>();
        light.type = LightType.Directional;
        light.intensity = 1.1f;
        light.transform.rotation = Quaternion.Euler(50, -30, 0);
        // Flat ambient (the default, Skybox, ignores ambientLight and greys the dark ground).
        RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Flat;
        RenderSettings.ambientLight = new Color(0.25f, 0.28f, 0.38f);

        Material Mat(Color c, float gloss = 0.35f)
        {
            var m = baseMaterial ? new Material(baseMaterial) : new Material(Shader.Find("Standard"));
            m.color = c;
            if (m.HasProperty("_Glossiness")) m.SetFloat("_Glossiness", gloss);
            return m;
        }

        var ground = GameObject.CreatePrimitive(PrimitiveType.Plane);
        ground.name = "Ground";
        ground.transform.localScale = new Vector3(4, 1, 4);  // 40 x 40
        ground.GetComponent<Renderer>().sharedMaterial = Mat(new Color32(14, 19, 34, 255), 0);  // matte: no sky reflection

        var tower = GameObject.CreatePrimitive(PrimitiveType.Cylinder);
        tower.name = "Tower";
        tower.transform.position = new Vector3(0, 1.1f, 0);
        tower.transform.localScale = new Vector3(2.1f, 1.1f, 2.1f);
        tower.GetComponent<Renderer>().sharedMaterial = Mat(new Color32(26, 36, 64, 255));

        materials_ = new Material[kHues.Length];
        for (int i = 0; i < kHues.Length; i++) materials_[i] = Mat(kHues[i]);

        // Selection: the selected cube gets a material of its own, lit
        // towards white and pulsing, and a glowing disc under it.
        selectedMaterial_ = Mat(Color.white, 0.6f);
        ringMaterial_ = Mat(Color.white, 0);
        var ring = GameObject.CreatePrimitive(PrimitiveType.Cylinder);
        ring.name = "SelectionRing";
        Destroy(ring.GetComponent<Collider>());
        ring.GetComponent<Renderer>().sharedMaterial = ringMaterial_;
        ring_ = ring.transform;
        ring.SetActive(false);
    }

    void Select(int index)
    {
        if (index != selected_) Debug.Log(index >= 0 ? $"selected: UNIT {index + 1:00}" : "selection cleared");
        if (selected_ >= 0 && selected_ < units_.Count)
            units_[selected_].body.GetComponent<Renderer>().sharedMaterial = materials_[units_[selected_].hue];
        selected_ = index;
        if (selected_ >= 0)
        {
            units_[selected_].body.GetComponent<Renderer>().sharedMaterial = selectedMaterial_;
            ringMaterial_.color = Color.Lerp(kHues[units_[selected_].hue], Color.white, 0.25f);
        }
        ring_.gameObject.SetActive(selected_ >= 0);
    }

    // The unit under `ui` (UI px), nearest first: the camera ray through that
    // point against each cube, in the cube's own (rotated, scaled) space.
    public int Pick(Vector2 ui)
    {
        var ray = camera_.ScreenPointToRay(new Vector3(ui.x * view_.PixelRatio, Screen.height - ui.y * view_.PixelRatio, 0));
        var box = new Bounds(Vector3.zero, Vector3.one);
        int best = -1;
        float bestDistance = 0;
        for (int i = 0; i < units_.Count; i++)
        {
            var body = units_[i].body;
            var local = new Ray(body.InverseTransformPoint(ray.origin), body.InverseTransformDirection(ray.direction));
            if (!box.IntersectRay(local, out float d)) continue;
            float distance = Vector3.Distance(ray.origin, body.TransformPoint(local.GetPoint(d)));
            if (best < 0 || distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
        }
        return best;
    }

    void Spawn()
    {
        if (units_.Count >= 48)
        {
            if (selected_ == 0) Select(-1);
            Destroy(units_[0].body.gameObject);
            units_.RemoveAt(0);
            if (selected_ > 0) selected_--;
        }
        var u = new Unit
        {
            angle = Random.Range(0f, 6.28f),
            radius = 2.5f + Random.Range(0f, 7f),
            speed = (Random.value < 0.5f ? 1f : -1f) * (0.2f + Random.Range(0f, 0.6f)),
            height = 0.5f + Random.Range(0f, 1.5f),
            size = 0.6f + Random.Range(0f, 0.6f),
            phase = Random.Range(0f, 6.28f),
            hue = spawned_++ % kHues.Length,
        };
        var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
        cube.name = "Unit";
        Destroy(cube.GetComponent<Collider>());
        cube.GetComponent<Renderer>().sharedMaterial = materials_[u.hue];
        cube.transform.localScale = Vector3.one * u.size;
        u.body = cube.transform;
        units_.Add(u);
    }

    void Clear()
    {
        Select(-1);
        foreach (var u in units_) Destroy(u.body.gameObject);
        units_.Clear();
    }

    Vector3 UnitPos(Unit u) =>
        new Vector3(Mathf.Cos(u.angle) * u.radius, u.height + 0.25f * Mathf.Sin(time_ * 2f + u.phase), Mathf.Sin(u.angle) * u.radius);

    // ------------------------------------------------------------ frame

    void Update()
    {
        // Orbit while a press the UI passed on is held (UI px, y down).
        if (dragging_)
        {
            var p = view_.PointerPosition;
            yaw_ -= (p.x - lastPointer_.x) * 0.006f;
            pitch_ += (p.y - lastPointer_.y) * 0.006f;
            travel_ = Mathf.Max(travel_, Vector2.Distance(p, pressAt_));
            lastPointer_ = p;
        }

        float dt = frames_ >= 0 || selftest_ ? 1f / 60f : Time.deltaTime;  // deterministic runs
        if (!paused_)
        {
            time_ += dt;
            foreach (var u in units_) u.angle += u.speed * dt;
        }
        pitch_ = Mathf.Clamp(pitch_, 0.15f, 1.35f);
        camera_.transform.position = new Vector3(Mathf.Cos(yaw_) * Mathf.Cos(pitch_), Mathf.Sin(pitch_), Mathf.Sin(yaw_) * Mathf.Cos(pitch_)) * distance_;
        camera_.transform.LookAt(new Vector3(0, 0.8f, 0));
        foreach (var u in units_)
        {
            u.body.position = UnitPos(u);
            u.body.rotation = Quaternion.Euler(0, u.angle * Mathf.Rad2Deg, 0);
        }
        if (selected_ >= 0)
        {
            var u = units_[selected_];
            selectedMaterial_.color = Color.Lerp(kHues[u.hue], Color.white, 0.35f + 0.15f * Mathf.Sin(time_ * 6f));
            var at = u.body.position;
            float r = u.size * (2.1f + 0.15f * Mathf.Sin(time_ * 4f));
            ring_.position = new Vector3(at.x, 0.02f, at.z);
            ring_.localScale = new Vector3(r, 0.01f, r);
        }

        PublishFrameData();
        CountFrame(Time.realtimeSinceStartupAsDouble);
        if (frameStats_) FrameStats();
        PublishState();
    }

    // --frame-stats: every 10 s, real (not smoothed) frame times bucketed by
    // display refresh (on time, one refresh missed, more), their mean / min /
    // max, and how long the main thread waited for JSUI's GPU renders.
    readonly int[] statBuckets_ = new int[4];
    double statStart_ = -1, lastFrameAt_, statSumMs_, statMinMs_ = double.MaxValue, statMaxMs_, statWaitMs_, statWaitMax_;
    int statFrames_;

    void FrameStats()
    {
        double now = Time.realtimeSinceStartupAsDouble;  // Time.unscaledDeltaTime is smoothed under vSync
        double ms = (now - lastFrameAt_) * 1000;
        lastFrameAt_ = now;
        double wait = view_.TakeRenderWaitMs();
        if (statStart_ < 0 || framesSeen_ < kFpsWarmup)
        {
            statStart_ = now;
            return;
        }
        double refresh = 1000 / Screen.currentResolution.refreshRateRatio.value;
        statBuckets_[ms < refresh * 0.5 ? 0 : ms < refresh * 1.5 ? 1 : ms < refresh * 2.5 ? 2 : 3]++;
        statFrames_++;
        statSumMs_ += ms;
        statMinMs_ = Math.Min(statMinMs_, ms);
        statMaxMs_ = Math.Max(statMaxMs_, ms);
        statWaitMs_ += wait;
        statWaitMax_ = Math.Max(statWaitMax_, wait);
        if (now - statStart_ < 10) return;
        Debug.Log(string.Format(CultureInfo.InvariantCulture,
            "frame-stats: {0} frames in {1:0.0} s = {2:0.00} fps (display {3:0.00} Hz); frame time mean {4:0.000} ms, min {5:0.00}, max {6:0.00}; " +
            "<{7:0.0} ms: {8}, ~{9:0.0} ms: {10}, ~{11:0.0} ms: {12}, longer: {13}; jsuiu_wait_render {14:0.000} ms/frame (max {15:0.00}); " +
            "vSyncCount {16}, targetFrameRate {17}",
            statFrames_, now - statStart_, statFrames_ / (now - statStart_), 1000 / refresh, statSumMs_ / statFrames_, statMinMs_, statMaxMs_,
            refresh * 0.5, statBuckets_[0], refresh, statBuckets_[1], refresh * 2, statBuckets_[2], statBuckets_[3],
            statWaitMs_ / statFrames_, statWaitMax_, QualitySettings.vSyncCount, Application.targetFrameRate));
        Array.Clear(statBuckets_, 0, statBuckets_.Length);
        statStart_ = now;
        statFrames_ = 0;
        statSumMs_ = statMaxMs_ = statWaitMs_ = statWaitMax_ = 0;
        statMinMs_ = double.MaxValue;
    }

    // After JsuiView.LateUpdate ticked (the game's execution order is later).
    void LateUpdate()
    {
        frame_++;
        if (test_ != null)
        {
            var size = view_.Size;
            int r = test_.Step(view_.Runtime, this, size.x, size.y);
            if (r != 0) Finish(r > 0 ? 0 : 1);
        }
        if (frames_ >= 0 && frame_ >= frames_) Finish(0);
    }

    void CountFrame(double now)
    {
        if (framesSeen_++ < kFpsWarmup) return;
        long n = framesSeen_ - kFpsWarmup;  // frames in the ring, up to kFpsWindow
        frameTimes_[(n - 1) % kFpsWindow] = now;
        if (n < 20) return;
        double oldest = frameTimes_[n < kFpsWindow ? 0 : n % kFpsWindow];
        long count = n < kFpsWindow ? n : kFpsWindow;
        if (now <= oldest || (fpsShownAt_ >= 0 && now - fpsShownAt_ < 0.25)) return;
        fps_ = (int)Math.Round((count - 1) / (now - oldest));
        fpsShownAt_ = now;
    }

    // Unit screen positions for the nameplates: x, y, visible, hue (logical px).
    void PublishFrameData()
    {
        var size = view_.Size;
        for (int i = 0; i < units_.Count; i++)
        {
            var u = units_[i];
            var p = UnitPos(u);
            p.y += u.size * 0.5f + 0.35f;
            var s = view_.WorldToUi(camera_, p);
            bool visible = s.z > 0.1f && s.x > -60 && s.y > -60 && s.x < size.x + 60 && s.y < size.y + 60;
            entities_[i * 4 + 0] = Mathf.Round(s.x);
            entities_[i * 4 + 1] = Mathf.Round(s.y);
            entities_[i * 4 + 2] = visible ? 1 : 0;
            entities_[i * 4 + 3] = i % 7;
        }
        view_.SetBuffer("entities", entities_, units_.Count * 4);
    }

    void PublishState()
    {
        var json = string.Format(CultureInfo.InvariantCulture,
            "{{\"engine\":\"Unity {0}\",\"renderer\":\"{1}\",\"fps\":{2},\"entities\":{3},\"paused\":{4}," +
            "\"selected\":{8},\"passed\":{{\"clicks\":{5},\"wheel\":{6},\"keys\":{7},\"pad\":0}}}}",
            Application.unityVersion, view_.RendererName, fps_, units_.Count,
            paused_ ? "true" : "false", clicks_, wheel_, keys_, selected_);
        if (json == lastState_) return;
        lastState_ = json;
        view_.SetState("game", json);
    }

    void OnUiMessage(string name, string json)
    {
        test_?.OnMessage(name, json);
        switch (name)
        {
            case "game:spawn":
                int n = Mathf.Clamp((int)SelfTest.JsonNumber(json, "count", 1), 1, 50);
                for (int i = 0; i < n; i++) Spawn();
                break;
            case "game:clear":
                Clear();
                break;
            case "game:pause":
                paused_ = json != null && json.Contains("\"paused\":true");
                break;
            case "game:quit":
                Finish(0);
                break;
        }
    }

    // ------------------------------------------------------------ SelfTest.IHost

    public int Entities => units_.Count;
    public int Selected => selected_;
    public int Clicks => clicks_;

    // A left click at `ui` through JsuiView.HandlePointer, the entry point
    // Unity's Input goes through each frame: move, press, release.
    public void Click(Vector2 ui)
    {
        var screen = new Vector2(ui.x * view_.PixelRatio, Screen.height - ui.y * view_.PixelRatio);
        view_.HandlePointer(new JsuiView.PointerFrame { position = screen });
        view_.HandlePointer(new JsuiView.PointerFrame { position = screen, pressed = 1 });
        view_.HandlePointer(new JsuiView.PointerFrame { position = screen, released = 1 });
    }

    public bool UnitOnScreen(int i, out Vector2 ui)
    {
        ui = Vector2.zero;
        if (i < 0 || i >= units_.Count) return false;
        var s = view_.WorldToUi(camera_, units_[i].body.position);
        ui = new Vector2(s.x, s.y);
        var size = view_.Size;
        return s.z > 0.1f && s.x >= 0 && s.y >= 0 && s.x < size.x && s.y < size.y;
    }

    // ------------------------------------------------------------ exit

    bool finishing_;

    void Finish(int code)
    {
        if (finishing_) return;
        finishing_ = true;
        if (!string.IsNullOrEmpty(screenshot_)) StartCoroutine(ScreenshotAndQuit(code));
        else Quit(code);
    }

    // The composed frame (scene + UI overlay), read back at the end of the frame.
    IEnumerator ScreenshotAndQuit(int code)
    {
        yield return new WaitForEndOfFrame();
        var tex = ScreenCapture.CaptureScreenshotAsTexture();
        try
        {
            File.WriteAllBytes(screenshot_, tex.EncodeToPNG());
            Debug.Log($"screenshot: {screenshot_} ({tex.width}x{tex.height})");
        }
        catch (Exception e)
        {
            Debug.LogError($"screenshot failed: {e.Message}");
            code = 1;
        }
        Destroy(tex);
        Quit(code);
    }

    static void Quit(int code)
    {
#if UNITY_EDITOR
        UnityEditor.EditorApplication.isPlaying = false;
#else
        Application.Quit(code);
#endif
    }
}
