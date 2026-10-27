// Test bundle: a selectable Text (y 0..40) over a TextInput (y 60..90), for
// the clipboard test. Change / selection events go to the host as messages.
(function () {
  const N = globalThis.__jsui_native;
  const text = N.createNode('Text');
  N.setProps(text, { selectable: true, __events: ['selectionChange'],
    style: { position: 'absolute', left: 10, top: 10, width: 300, fontSize: 20 } });
  const raw = N.createNode('RawText');
  N.setText(raw, 'alpha beta gamma');
  N.appendChild(text, raw);
  N.appendChild(N.rootId, text);
  const input = N.createNode('TextInput');
  N.setProps(input, { __events: ['change'],
    style: { position: 'absolute', left: 10, top: 60, width: 300, height: 30, fontSize: 16 } });
  N.appendChild(N.rootId, input);
  globalThis.__jsui_dispatchEvent = function (ev) {
    if (ev.type === 'change') N.postMessage('changed', JSON.stringify({ text: ev.text }));
    if (ev.type === 'selectionchange' && ev.target === text) N.postMessage('selected', JSON.stringify({ text: ev.text }));
    return false;
  };
})();
