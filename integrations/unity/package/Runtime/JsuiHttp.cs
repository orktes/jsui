// JsuiHttp: a view's network requests (fetch(), images, fonts and sounds
// from http:// and https:// URLs) on UnityWebRequest, the engine's own HTTP
// stack, so a game's UI uses the same TLS, proxies and platform rules as the
// rest of the game. The plugin queues the requests (JsuiConfig.on_request,
// native/jsui_unity.cpp); JsuiView calls Update after each tick, which
// starts the new ones, aborts the ones JS dropped (AbortController, unmount)
// and finishes the completed ones. Body bytes stream back to JS as Unity
// receives them (a DownloadHandlerScript), so res.body readers see them as
// they arrive.
//
// Redirects: UnityWebRequest follows them itself (redirectLimit 20) for
// redirect: 'follow'; for 'error' and 'manual' it is told not to (0) and the
// 3xx either fails the request or is handed to JS as is.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine.Networking;

namespace Jsui
{
    sealed class JsuiHttp : IDisposable
    {
        readonly IntPtr plugin_, rt_;
        readonly Dictionary<ulong, Transfer> live_ = new Dictionary<ulong, Transfer>();
        readonly List<ulong> finished_ = new List<ulong>();

        // The runtime is only handed answers (jsui_request_*, thread safe),
        // so a GPU render job using it meanwhile is no reason to wait.
        public JsuiHttp(IntPtr plugin, IntPtr rt)
        {
            plugin_ = plugin;
            rt_ = rt;
        }

        public void Update()
        {
            while (JsuiNative.jsuiu_next_request_cancel(plugin_, out ulong id) != 0)
            {
                if (!live_.TryGetValue(id, out var t)) continue;
                live_.Remove(id);
                t.Abort();
            }
            while (JsuiNative.jsuiu_next_request(plugin_, out ulong id, out IntPtr url, out IntPtr method, out IntPtr body,
                       out int bodySize, out int timeoutMs, out int redirect, out int headerCount) != 0)
            {
                var headers = new List<KeyValuePair<string, string>>(headerCount);
                for (int i = 0; i < headerCount; i++)
                    if (JsuiNative.jsuiu_request_header(plugin_, i, out IntPtr name, out IntPtr value) != 0)
                        headers.Add(new KeyValuePair<string, string>(JsuiNative.FromUtf8(name), JsuiNative.FromUtf8(value)));
                byte[] bytes = null;
                if (body != IntPtr.Zero && bodySize > 0)
                {
                    bytes = new byte[bodySize];
                    Marshal.Copy(body, bytes, 0, bodySize);
                }
                var t = new Transfer(plugin_, rt_, id, redirect);
                live_[id] = t;
                t.Start(JsuiNative.FromUtf8(url), JsuiNative.FromUtf8(method), headers, bytes, timeoutMs);
            }
            foreach (var kv in live_)
                if (kv.Value.Poll()) finished_.Add(kv.Key);
            foreach (var id in finished_)
            {
                live_[id].Release();
                live_.Remove(id);
            }
            finished_.Clear();
        }

        // The runtime is going away: drop every request unanswered.
        public void Dispose()
        {
            foreach (var t in live_.Values) t.Abort();
            live_.Clear();
        }

        sealed class Transfer : DownloadHandlerScript
        {
            readonly IntPtr plugin_, rt_;
            readonly ulong id_;
            readonly int redirect_;
            UnityWebRequest request_;
            string url_;
            bool responded_, done_;

            public Transfer(IntPtr plugin, IntPtr rt, ulong id, int redirect) : base(new byte[64 * 1024])
            {
                plugin_ = plugin;
                rt_ = rt;
                id_ = id;
                redirect_ = redirect;
            }

            public void Start(string url, string method, List<KeyValuePair<string, string>> headers, byte[] body, int timeoutMs)
            {
                url_ = url;
                try
                {
                    request_ = new UnityWebRequest(url, method) { downloadHandler = this };
                }
                catch (Exception e)
                {
                    Fail(e.Message);
                    return;
                }
                request_.redirectLimit = redirect_ == JsuiNative.RedirectFollow ? 20 : 0;
                if (timeoutMs > 0) request_.timeout = (timeoutMs + 999) / 1000;
                string contentType = null;
                bool hasUserAgent = false;
                foreach (var h in headers)
                {
                    // Framing headers are UnityWebRequest's to set.
                    switch (h.Key)
                    {
                        case "host": case "content-length": case "connection": case "transfer-encoding": case "accept-encoding":
                            continue;
                        case "content-type": contentType = h.Value; break;
                        case "user-agent": hasUserAgent = true; break;
                    }
                    SetHeader(h.Key, h.Value);
                }
                if (!hasUserAgent) SetHeader("user-agent", "jsui/0.1");
                if (body != null)
                    request_.uploadHandler = new UploadHandlerRaw(body) { contentType = contentType ?? "application/octet-stream" };
                request_.SendWebRequest();
            }

            // Some platforms refuse some headers (the browser's forbidden
            // ones on WebGL): the request goes without them.
            void SetHeader(string name, string value)
            {
                try
                {
                    request_.SetRequestHeader(name, value);
                }
                catch (Exception) { }
            }

            protected override bool ReceiveData(byte[] data, int dataLength)
            {
                if (done_ || !Respond()) return false;
                if (dataLength > 0) JsuiNative.jsui_request_body(rt_, id_, data, (UIntPtr)dataLength);
                return true;
            }

            // Whether the request has ended (answered in full, or failed).
            public bool Poll()
            {
                if (done_) return true;
                if (request_ == null || !request_.isDone) return false;
                long code = request_.responseCode;
                bool redirect = code >= 300 && code < 400 && redirect_ != JsuiNative.RedirectFollow;
                if (request_.result == UnityWebRequest.Result.Success || request_.result == UnityWebRequest.Result.ProtocolError ||
                    redirect)
                {
                    // A 3xx with redirectLimit 0 may also count as an error
                    // ("redirect limit reached"): it is the answer here.
                    if (Respond()) JsuiNative.RequestEnd(rt_, id_, null);
                }
                else
                {
                    var error = string.IsNullOrEmpty(request_.error) ? "network error" : request_.error;
                    if (responded_) JsuiNative.RequestEnd(rt_, id_, error);
                    else Fail(error);
                }
                done_ = true;
                return true;
            }

            // Sends the status and headers once; false when the request failed
            // instead (a redirect with redirect: 'error').
            bool Respond()
            {
                if (responded_) return true;
                if (done_) return false;
                int code = (int)request_.responseCode;
                var headers = request_.GetResponseHeaders();
                string location = null;
                if (headers != null)
                    foreach (var kv in headers)
                        if (string.Equals(kv.Key, "location", StringComparison.OrdinalIgnoreCase)) location = kv.Value;
                if (redirect_ == JsuiNative.RedirectError && code >= 300 && code < 400 && location != null)
                {
                    try
                    {
                        location = new Uri(new Uri(url_), location).AbsoluteUri;
                    }
                    catch (Exception) { }
                    Fail("redirect to " + location + " with redirect: 'error'");
                    return false;
                }
                // UnityWebRequest asks for gzip and decodes it itself: the
                // reported encoding and length would describe bytes JS never sees.
                string encoding = null;
                if (headers != null)
                    foreach (var kv in headers)
                        if (string.Equals(kv.Key, "content-encoding", StringComparison.OrdinalIgnoreCase)) encoding = kv.Value;
                bool decoded = encoding != null && !string.Equals(encoding.Trim(), "identity", StringComparison.OrdinalIgnoreCase);
                var lines = new StringBuilder();
                if (headers != null)
                    foreach (var kv in headers)
                    {
                        if (decoded && (string.Equals(kv.Key, "content-encoding", StringComparison.OrdinalIgnoreCase) ||
                                        string.Equals(kv.Key, "content-length", StringComparison.OrdinalIgnoreCase)))
                            continue;
                        if (kv.Key.IndexOf('\n') < 0 && kv.Value.IndexOf('\n') < 0)
                            lines.Append(kv.Key).Append(": ").Append(kv.Value).Append('\n');
                    }
                responded_ = true;
                // No reason phrase from UnityWebRequest.
                JsuiNative.RequestRespond(plugin_, id_, code, null, request_.url, lines.ToString());
                return true;
            }

            void Fail(string error)
            {
                done_ = true;
                JsuiNative.RequestRespond(plugin_, id_, 0, error, null, null);
            }

            public void Abort()
            {
                done_ = true;
                request_?.Abort();
                Release();
            }

            // Disposing the request disposes this handler too.
            public void Release()
            {
                var r = request_;
                request_ = null;
                if (r != null) r.Dispose();
                else Dispose();
            }
        }
    }
}
