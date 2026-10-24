// Test bundle for UI audio (web/jsui-audio.js): a solid view, and host
// messages that drive the __jsui_native audio bridge. Reports
// (__jsui_onAudio) go back to the host as 'audio' messages.
(function () {
  const N = globalThis.__jsui_native;
  const id = N.createNode('View');
  N.setProps(id, { style: { width: 320, height: 200, backgroundColor: '#204060' } });
  N.appendChild(N.rootId, id);
  globalThis.__jsui_onAudio = (ev) => N.postMessage('audio', JSON.stringify(ev));
  globalThis.__jsui_onHostMessage = function (name, json) {
    const a = JSON.parse(json);
    if (name === 'play') {
      const v = N.audioPlay(a.url, a.bus || 'sfx', a.volume ?? 1, a.pitch ?? 1, 0, !!a.loop, a.fade || 0, 0);
      N.postMessage('voice', JSON.stringify({ tag: a.tag, voice: v }));
    } else if (name === 'stop') N.audioVoice(3, a.voice, 1, 1, 0, a.fade || 0);
    else if (name === 'preload') N.audioPreload(a.url);
    else if (name === 'bus') N.audioBus(a.bus, a.volume, !!a.muted, 0);
  };
  N.postMessage('ready', '{}');
})();
