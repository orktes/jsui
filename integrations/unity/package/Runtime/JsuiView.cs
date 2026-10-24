// JsuiView: a JSUI runtime drawn over the game as a full-screen uGUI layer.
//
// Add it to any GameObject. It creates its own Screen Space - Overlay canvas
// with a RawImage showing the UI texture, loads the bundle, forwards mouse
// and keyboard input and ticks the runtime once per frame.
//
// Rendering. The runtime rasterises on the CPU into the plugin's buffer
// (jsui_render_rgba_incremental: only what changed is repainted) and the
// view uploads it into a Texture2D when anything changed. The texture is
// premultiplied RGBA; the RawImage draws it with a premultiplied-alpha
// shader (Resources/JsuiPremultiplied.shader), flipped, since the rows are
// top-down and Unity textures are bottom-up.
//
// Input pass-through. Every mouse and keyboard event goes to the UI first;
// jsui_send_* says whether the UI consumed it. Events the UI did not consume
// are raised as the Passed* events below, so the game reacts only to input
// that was not meant for the UI. JsuiView runs before other scripts
// (DefaultExecutionOrder), so those events fire before the game's Update.
//
// Game <-> UI. Messages from JS (Host.postMessage) arrive as OnMessage after
// each tick; SetState / PostMessage / MapBuffer + CommitBuffer feed the UI
// (useHostState, Host.addListener, useHostBufferValue).
using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.UI;

namespace Jsui
{
    [DefaultExecutionOrder(-1000)]
    public class JsuiView : MonoBehaviour
    {
        [Tooltip("Bundle path relative to StreamingAssets, an absolute path, or an http:// URL (jsui dev)")]
        public string bundle = "jsui/bundle.js";
        [Tooltip("Poll the dev server and hot-update (use with an http:// bundle from `npx jsui dev`)")]
        public bool devMode;
        [Tooltip("Device pixels per logical px; 0 picks 2 on high-DPI screens, else 1")]
        public float pixelRatio;
        [Tooltip("Sorting order of the overlay canvas")]
        public int sortingOrder = 100;
        [Tooltip("Forward mouse and keyboard input to the UI")]
        public bool forwardInput = true;

        /// <summary>Host.postMessage(name, payload) from JS: name and the payload as JSON.</summary>
        public event Action<string, string> OnMessage;
        /// <summary>A mouse press the UI did not consume: button (0 left, 1 right, 2 middle as in Unity) and position (logical px, top-left origin).</summary>
        public event Action<int, Vector2> PassedMouseDown;
        /// <summary>A wheel movement the UI did not consume (Unity's Input.mouseScrollDelta).</summary>
        public event Action<Vector2> PassedWheel;
        /// <summary>A key press the UI did not consume.</summary>
        public event Action<KeyCode> PassedKeyDown;

        public IntPtr Runtime => rt_;
        /// <summary>Viewport size in logical px.</summary>
        public Vector2 Size => new Vector2(width_, height_);
        public float PixelRatio => ratio_;
        /// <summary>Whether the last left press went to the UI (so a drag that started there is not the game's).</summary>
        public bool PointerCaptured { get; private set; }

        IntPtr plugin_, rt_;
        Canvas canvas_;
        RawImage image_;
        Texture2D texture_;
        Material material_;
        int width_, height_;
        float ratio_ = 1;
        Vector2 mouse_ = new Vector2(float.NaN, float.NaN);
        bool mouseInside_;
        readonly bool[] buttonDown_ = new bool[3];
        readonly HashSet<KeyCode> keysDown_ = new HashSet<KeyCode>();
        bool forceRepaint_ = true;

        void Awake()
        {
            ratio_ = pixelRatio > 0 ? pixelRatio : (Screen.dpi >= 160 ? 2 : 1);
            width_ = Mathf.Max(1, Mathf.RoundToInt(Screen.width / ratio_));
            height_ = Mathf.Max(1, Mathf.RoundToInt(Screen.height / ratio_));
            plugin_ = JsuiNative.jsuiu_create(width_, height_, ratio_, devMode ? 1 : 0);
            if (plugin_ == IntPtr.Zero)
            {
                Debug.LogError("JSUI: jsuiu_create failed");
                enabled = false;
                return;
            }
            rt_ = JsuiNative.jsuiu_runtime(plugin_);
            CreateCanvas();
        }

        void Start()
        {
            // Load after every Awake, so the game's first SetState calls are
            // in place when the bundle's first render runs.
            var url = ResolveBundle(bundle);
            if (JsuiNative.LoadBundleUrl(rt_, url) != 0)
                Debug.LogError($"JSUI: failed to load {url}: {JsuiNative.LastError(rt_)}");
        }

        void OnDestroy()
        {
            if (plugin_ != IntPtr.Zero) JsuiNative.jsuiu_destroy(plugin_);
            plugin_ = rt_ = IntPtr.Zero;
            if (texture_) Destroy(texture_);
            if (material_) Destroy(material_);
            if (canvas_) Destroy(canvas_.gameObject);
        }

        static string ResolveBundle(string b)
        {
            if (b.Contains("://")) return b;
            var path = Path.IsPathRooted(b) ? b : Path.Combine(Application.streamingAssetsPath, b);
            return new Uri(Path.GetFullPath(path)).AbsoluteUri;
        }

        void CreateCanvas()
        {
            var go = new GameObject("JSUI Canvas");
            go.transform.SetParent(transform, false);
            canvas_ = go.AddComponent<Canvas>();
            canvas_.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas_.sortingOrder = sortingOrder;
            var imageGo = new GameObject("JSUI");
            imageGo.transform.SetParent(go.transform, false);
            image_ = imageGo.AddComponent<RawImage>();
            image_.raycastTarget = false;  // JSUI does its own hit testing
            var rect = image_.rectTransform;
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = rect.offsetMax = Vector2.zero;
            image_.uvRect = new Rect(0, 1, 1, -1);  // rows are top-down
            var shader = Resources.Load<Shader>("JsuiPremultiplied");
            if (shader) image_.material = material_ = new Material(shader);
            else Debug.LogWarning("JSUI: JsuiPremultiplied shader missing; edges will look dark");
        }

        // ------------------------------------------------------------ API

        public void SetState(string key, string json) => JsuiNative.SetState(rt_, key, json);
        public void PostMessage(string name, string json) => JsuiNative.PostMessage(rt_, name, json);

        /// <summary>Writes `values` into the frame buffer `key` (Float32Array in JS) and commits it.</summary>
        public unsafe void SetBuffer(string key, float[] values, int count)
        {
            var p = (float*)JsuiNative.MapBuffer(rt_, key, JsuiNative.BufferF32, count);
            if (p == null) return;
            for (int i = 0; i < count; i++) p[i] = values[i];
            JsuiNative.CommitBuffer(rt_, key);
        }

        /// <summary>Screen position (pixels, bottom-left origin) to UI coordinates (logical px, top-left origin).</summary>
        public Vector2 ScreenToUi(Vector2 screen) => new Vector2(screen.x / ratio_, (Screen.height - screen.y) / ratio_);
        /// <summary>World position to UI coordinates via `cam`; z &lt; 0 when behind the camera.</summary>
        public Vector3 WorldToUi(Camera cam, Vector3 world)
        {
            var s = cam.WorldToScreenPoint(world);
            return new Vector3(s.x / ratio_, (Screen.height - s.y) / ratio_, s.z);
        }

        // ---------------------------------------------------------- frame

        void Update()
        {
            if (rt_ == IntPtr.Zero) return;
            ResizeIfNeeded();
            if (forwardInput) ForwardPointer();
        }

        void LateUpdate()
        {
            if (rt_ == IntPtr.Zero) return;
            JsuiNative.jsui_tick(rt_, Time.realtimeSinceStartupAsDouble * 1000.0);
            DrainQueues();
            Paint();
        }

        void ResizeIfNeeded()
        {
            int w = Mathf.Max(1, Mathf.RoundToInt(Screen.width / ratio_));
            int h = Mathf.Max(1, Mathf.RoundToInt(Screen.height / ratio_));
            if (w == width_ && h == height_) return;
            width_ = w;
            height_ = h;
            JsuiNative.jsui_set_viewport(rt_, w, h, ratio_);
        }

        void DrainQueues()
        {
            while (JsuiNative.jsuiu_next_log(plugin_, out int level, out IntPtr text) != 0)
            {
                var s = "JSUI: " + JsuiNative.FromUtf8(text);
                if (level >= 3) Debug.LogError(s);
                else if (level == 2) Debug.LogWarning(s);
                else Debug.Log(s);
            }
            while (JsuiNative.jsuiu_next_message(plugin_, out IntPtr name, out IntPtr json) != 0)
                OnMessage?.Invoke(JsuiNative.FromUtf8(name), JsuiNative.FromUtf8(json));
        }

        void Paint()
        {
            int pw = Mathf.Max(1, Mathf.RoundToInt(width_ * ratio_));
            int ph = Mathf.Max(1, Mathf.RoundToInt(height_ * ratio_));
            if (texture_ == null || texture_.width != pw || texture_.height != ph)
            {
                if (texture_) Destroy(texture_);
                // sRGB texture: in Linear projects the sampler decodes the
                // UI's sRGB colours; in Gamma projects nothing converts.
                texture_ = new Texture2D(pw, ph, TextureFormat.RGBA32, false, false)
                {
                    name = "JSUI",
                    filterMode = FilterMode.Bilinear,
                    wrapMode = TextureWrapMode.Clamp,
                };
                image_.texture = texture_;
                forceRepaint_ = true;
            }
            if (JsuiNative.jsuiu_render(plugin_, pw, ph, forceRepaint_ ? 1 : 0, out JsuiRect dirty, out IntPtr pixels) == 0)
                return;
            forceRepaint_ = false;
            // Texture2D has no sub-rectangle upload: the whole buffer goes up
            // (only on frames where something changed).
            texture_.LoadRawTextureData(pixels, pw * ph * 4);
            texture_.Apply(false, false);
        }

        // ---------------------------------------------------------- input

        int Mods()
        {
            int m = 0;
            if (Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift)) m |= JsuiNative.ModShift;
            if (Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl)) m |= JsuiNative.ModCtrl;
            if (Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt)) m |= JsuiNative.ModAlt;
            if (Input.GetKey(KeyCode.LeftCommand) || Input.GetKey(KeyCode.RightCommand) ||
                Input.GetKey(KeyCode.LeftWindows) || Input.GetKey(KeyCode.RightWindows)) m |= JsuiNative.ModMeta;
            return m;
        }

        // Unity mouse button (0 left, 1 right, 2 middle) -> JsuiMouseButton.
        static readonly int[] kButtons = { JsuiNative.MouseLeft, JsuiNative.MouseRight, JsuiNative.MouseMiddle };

        void ForwardPointer()
        {
            if (!Input.mousePresent) return;
            var screen = (Vector2)Input.mousePosition;
            bool inside = screen.x >= 0 && screen.y >= 0 && screen.x < Screen.width && screen.y < Screen.height;
            var p = ScreenToUi(screen);
            if (inside && p != mouse_)
            {
                JsuiNative.jsui_send_mouse_move(rt_, p.x, p.y);
                mouse_ = p;
            }
            else if (!inside && mouseInside_ && !buttonDown_[0] && !buttonDown_[1] && !buttonDown_[2])
            {
                JsuiNative.jsui_send_mouse_leave(rt_);
                mouse_ = new Vector2(float.NaN, float.NaN);
            }
            mouseInside_ = inside;

            for (int b = 0; b < 3; b++)
            {
                if (Input.GetMouseButtonDown(b) && inside)
                {
                    buttonDown_[b] = true;
                    bool consumed = JsuiNative.jsui_send_mouse_button(rt_, p.x, p.y, kButtons[b], 1, Mods()) != 0;
                    if (b == 0) PointerCaptured = consumed;
                    if (!consumed) PassedMouseDown?.Invoke(b, p);
                }
                if (Input.GetMouseButtonUp(b) && buttonDown_[b])
                {
                    buttonDown_[b] = false;
                    JsuiNative.jsui_send_mouse_button(rt_, p.x, p.y, kButtons[b], 0, Mods());
                    if (b == 0) PointerCaptured = false;
                }
            }

            var wheel = Input.mouseScrollDelta;
            if (inside && wheel != Vector2.zero)
            {
                // DOM: positive deltaY scrolls down, ~40 px per notch.
                if (JsuiNative.jsui_send_mouse_wheel(rt_, p.x, p.y, -wheel.x * 40f, -wheel.y * 40f) == 0)
                    PassedWheel?.Invoke(wheel);
            }
        }

        // Keyboard and text come from IMGUI events: they carry OS key repeats
        // and the typed characters, which Input.GetKeyDown does not.
        void OnGUI()
        {
            if (rt_ == IntPtr.Zero || !forwardInput) return;
            var e = Event.current;
            if (e.type == EventType.KeyDown)
            {
                if (e.keyCode != KeyCode.None)
                {
                    bool repeat = !keysDown_.Add(e.keyCode);
                    var code = JsuiKeys.DomCode(e.keyCode);
                    var key = JsuiKeys.DomKey(e.keyCode, e.shift);
                    bool consumed = JsuiNative.SendKey(rt_, key, code, true, repeat, Mods()) != 0;
                    if (!consumed && !repeat) PassedKeyDown?.Invoke(e.keyCode);
                }
                // Typed text (a separate event with keyCode None on most
                // platforms); control characters are keys, not text.
                if (e.character != '\0' && !char.IsControl(e.character))
                    JsuiNative.SendText(rt_, e.character.ToString());
            }
            else if (e.type == EventType.KeyUp && e.keyCode != KeyCode.None)
            {
                keysDown_.Remove(e.keyCode);
                JsuiNative.SendKey(rt_, JsuiKeys.DomKey(e.keyCode, e.shift), JsuiKeys.DomCode(e.keyCode), false, false, Mods());
            }
        }

        void OnApplicationFocus(bool focused)
        {
            if (focused || rt_ == IntPtr.Zero) return;
            // Keys held when focus left never see their KeyUp.
            foreach (var k in keysDown_)
                JsuiNative.SendKey(rt_, JsuiKeys.DomKey(k, false), JsuiKeys.DomCode(k), false, false, 0);
            keysDown_.Clear();
        }
    }
}
