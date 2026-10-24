// P/Invoke declarations for the JSUI C API (include/jsui/jsui.h) and the
// plugin's own helpers (native/jsui_unity.cpp). Strings go in as UTF-8.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Jsui
{
    [StructLayout(LayoutKind.Sequential)]
    public struct JsuiRect
    {
        public int x, y, width, height;
    }

    public static class JsuiNative
    {
#if UNITY_IOS && !UNITY_EDITOR
        const string Lib = "__Internal";
#else
        const string Lib = "jsui_unity";
#endif

        public const int MouseLeft = 0, MouseMiddle = 1, MouseRight = 2;
        public const int ModShift = 1, ModCtrl = 2, ModAlt = 4, ModMeta = 8;
        public const int HitPointer = 1, HitScroll = 2;
        public const int BufferF32 = 0;

        // ---- plugin helpers

        [DllImport(Lib)] public static extern IntPtr jsuiu_create(int width, int height, float pixelRatio, int devMode);
        [DllImport(Lib)] public static extern IntPtr jsuiu_runtime(IntPtr u);
        [DllImport(Lib)] public static extern void jsuiu_destroy(IntPtr u);
        [DllImport(Lib)] public static extern int jsuiu_next_message(IntPtr u, out IntPtr name, out IntPtr json);
        [DllImport(Lib)] public static extern int jsuiu_next_log(IntPtr u, out int level, out IntPtr text);
        [DllImport(Lib)] public static extern int jsuiu_render(IntPtr u, int width, int height, int force, out JsuiRect dirty, out IntPtr pixels);
        // GPU path (see jsui_unity.cpp): render jobs on Unity's render thread.
        [DllImport(Lib)] public static extern int jsuiu_gpu_available();
        [DllImport(Lib)] public static extern int jsuiu_gpu_failed(IntPtr u);
        [DllImport(Lib)] public static extern void jsuiu_set_target(IntPtr u, IntPtr texture, int width, int height);
        [DllImport(Lib)] public static extern void jsuiu_invalidate_target(IntPtr u);
        [DllImport(Lib)] public static extern int jsuiu_queue_render(IntPtr u);
        [DllImport(Lib)] public static extern void jsuiu_wait_render(IntPtr u);
        [DllImport(Lib)] public static extern IntPtr jsuiu_render_event_func();

        // ---- runtime

        [DllImport(Lib)] static extern int jsui_load_bundle_url(IntPtr rt, byte[] url);
        public static int LoadBundleUrl(IntPtr rt, string url) => jsui_load_bundle_url(rt, Utf8(url));
        [DllImport(Lib)] public static extern void jsui_tick(IntPtr rt, double nowMs);
        [DllImport(Lib)] public static extern int jsui_needs_redraw(IntPtr rt);
        [DllImport(Lib)] public static extern int jsui_wants_frame(IntPtr rt);
        [DllImport(Lib)] public static extern void jsui_set_viewport(IntPtr rt, int width, int height, float pixelRatio);
        [DllImport(Lib)] static extern IntPtr jsui_last_error(IntPtr rt);
        public static string LastError(IntPtr rt) => FromUtf8(jsui_last_error(rt));
        [DllImport(Lib)] static extern IntPtr jsui_get_cursor(IntPtr rt);
        public static string Cursor(IntPtr rt) => FromUtf8(jsui_get_cursor(rt));

        // ---- game <-> UI

        [DllImport(Lib)] static extern void jsui_post_message(IntPtr rt, byte[] name, byte[] json);
        public static void PostMessage(IntPtr rt, string name, string json) => jsui_post_message(rt, Utf8(name), Utf8(json ?? "null"));
        [DllImport(Lib)] static extern int jsui_set_state(IntPtr rt, byte[] key, byte[] json);
        public static int SetState(IntPtr rt, string key, string json) => jsui_set_state(rt, Utf8(key), Utf8(json));
        [DllImport(Lib)] static extern IntPtr jsui_map_buffer(IntPtr rt, byte[] key, int type, UIntPtr count);
        public static IntPtr MapBuffer(IntPtr rt, string key, int type, int count) => jsui_map_buffer(rt, Utf8(key), type, (UIntPtr)count);
        [DllImport(Lib)] static extern int jsui_commit_buffer(IntPtr rt, byte[] key);
        public static int CommitBuffer(IntPtr rt, string key) => jsui_commit_buffer(rt, Utf8(key));

        // ---- input (each returns non-zero when the UI consumed the event)

        [DllImport(Lib)] public static extern int jsui_hit_test(IntPtr rt, float x, float y);
        [DllImport(Lib)] public static extern int jsui_send_mouse_move(IntPtr rt, float x, float y);
        [DllImport(Lib)] public static extern int jsui_send_mouse_button(IntPtr rt, float x, float y, int button, int down, int mods);
        [DllImport(Lib)] public static extern int jsui_send_mouse_wheel(IntPtr rt, float x, float y, float dx, float dy);
        [DllImport(Lib)] public static extern int jsui_send_mouse_leave(IntPtr rt);
        [DllImport(Lib)] public static extern int jsui_send_touch(IntPtr rt, int id, int phase, float x, float y);
        [DllImport(Lib)] static extern int jsui_send_key(IntPtr rt, byte[] key, byte[] code, int down, int repeat, int mods);
        public static int SendKey(IntPtr rt, string key, string code, bool down, bool repeat, int mods) =>
            jsui_send_key(rt, Utf8(key), code != null ? Utf8(code) : null, down ? 1 : 0, repeat ? 1 : 0, mods);
        [DllImport(Lib)] static extern int jsui_send_text(IntPtr rt, byte[] utf8);
        public static int SendText(IntPtr rt, string text) => jsui_send_text(rt, Utf8(text));
        [DllImport(Lib)] public static extern int jsui_send_gamepad_button(IntPtr rt, int pad, int button, int down, float value);
        [DllImport(Lib)] public static extern int jsui_send_gamepad_axis(IntPtr rt, int pad, int axis, float value);
        [DllImport(Lib)] static extern void jsui_set_gamepad_connected(IntPtr rt, int pad, int connected, byte[] id);
        public static void SetGamepadConnected(IntPtr rt, int pad, bool connected, string id) =>
            jsui_set_gamepad_connected(rt, pad, connected ? 1 : 0, id != null ? Utf8(id) : null);

        // ---- strings

        public static byte[] Utf8(string s)
        {
            if (s == null) return null;
            var n = Encoding.UTF8.GetByteCount(s);
            var b = new byte[n + 1];  // NUL-terminated
            Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            return b;
        }

        public static string FromUtf8(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            int n = 0;
            while (Marshal.ReadByte(p, n) != 0) n++;
            if (n == 0) return "";
            var b = new byte[n];
            Marshal.Copy(p, b, 0, n);
            return Encoding.UTF8.GetString(b);
        }
    }
}
