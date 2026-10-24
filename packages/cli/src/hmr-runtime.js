// Hot module replacement client, prepended (as source text) to the bundles
// `jsui dev` serves. It runs in the JSUI runtime (QuickJS), not in Node.
//
// The dev bundle is: the vendor bundle (node_modules, @orktes/jsui-react and
// assets, built once by esbuild; it installs react-refresh's DevTools hook
// and fills globalThis.__jsui_hmr_vendor with lazy getters), this file, one
// __jsui_hmr.define() per app module, then __jsui_hmr.start(entry, version).
//
// When the runtime sees a new build (it polls /__jsui/version) it calls
// globalThis.__jsui_onDevUpdate(token). That fetches
// /__jsui/hmr?from=<applied version>, whose answer is a script: define()s of
// the changed modules and apply(ids, version), or reload(reason), or
// buildError(text).
//
// apply() re-executes each changed module and the importers above it, up to
// the nearest React Refresh boundaries (modules whose exports are all React
// components), then asks React Refresh to re-render: components keep their
// state. Reaching a module without importers (the entry) means nothing can
// take the update, so the runtime reloads the whole bundle instead.
(function () {
  'use strict';
  var G = globalThis;
  var Refresh = G.__jsui_hmr_refresh;
  var vendor = G.__jsui_hmr_vendor || {};
  var native = G.__jsui_native || {};
  var modules = Object.create(null);
  // Bare specifiers seen in any module (imports esbuild adds itself, such as
  // react/jsx-runtime, are not in every module's own list).
  var bare = Object.create(null);
  var version = null;
  var entryId = null;
  var updates = 0;

  function log(level, msg) {
    var c = G.console;
    if (c && c[level]) c[level]('[jsui] ' + msg);
  }

  function showError(text) {
    if (native.devError) native.devError(String(text));
    else log('error', text);
  }

  function clearError() {
    if (native.devError) native.devError(null);
  }

  function describe(e) {
    if (!e || typeof e !== 'object') return String(e);
    var head = (e.name || 'Error') + ': ' + e.message;
    var stack = e.stack ? String(e.stack) : '';
    return stack.indexOf(head) === 0 ? stack : head + (stack ? '\n' + stack : '');
  }

  function reload(reason) {
    log('log', 'full reload: ' + reason);
    if (native.reload) native.reload();
  }

  function define(id, deps, factory) {
    var m = modules[id];
    if (!m) {
      m = modules[id] = {
        id: id, deps: deps, factory: factory, loaded: false, failed: false, module: null,
        importers: new Set(), depIds: [], boundary: false, accepts: false, signature: null,
        hot: null, data: undefined,
      };
    } else {
      // The new code runs when apply() re-executes the module (or when it is
      // first required, if nothing loaded it yet).
      m.deps = deps;
      m.factory = factory;
    }
    for (var s in deps) if (s.charAt(0) !== '.' && s.charAt(0) !== '/') bare[s] = deps[s];
  }

  function requireFrom(m) {
    return function require(spec) {
      var id = Object.prototype.hasOwnProperty.call(m.deps, spec) ? m.deps[spec] : bare[spec];
      if (id === undefined) throw new Error("Cannot find module '" + spec + "' (imported by " + m.id + ')');
      if (m.depIds.indexOf(id) < 0) m.depIds.push(id);
      var dep = modules[id];
      if (dep) {
        dep.importers.add(m.id);
        if (!dep.loaded) execute(dep);
        return dep.module.exports;
      }
      var get = vendor[id];
      if (!get) throw new Error("Cannot find module '" + spec + "' (imported by " + m.id + ')');
      return get();
    };
  }

  function isComponent(v) {
    return !!Refresh && Refresh.isLikelyComponentType(v);
  }

  // Exports a refresh can swap in place: a component, or an object of only
  // components (plus __esModule).
  function exportKeys(exports) {
    if (exports == null || typeof exports !== 'object') return null;
    var keys = [];
    for (var k in exports) if (k !== '__esModule') keys.push(k);
    return keys;
  }

  function isBoundary(exports) {
    if (isComponent(exports)) return true;
    var keys = exportKeys(exports);
    if (!keys || !keys.length) return false;
    for (var i = 0; i < keys.length; i++) {
      var v;
      try {
        v = exports[keys[i]];
      } catch (e) {
        return false;
      }
      if (!isComponent(v)) return false;
    }
    return true;
  }

  function signatureOf(exports) {
    var keys = exportKeys(exports);
    return keys ? keys.sort().join(',') : typeof exports;
  }

  function registerExports(m) {
    var e = m.module.exports;
    if (isComponent(e)) Refresh.register(e, m.id + ' %exports%');
    var keys = exportKeys(e) || [];
    for (var i = 0; i < keys.length; i++) {
      var v;
      try {
        v = e[keys[i]];
      } catch (err) {
        continue;
      }
      if (isComponent(v)) Refresh.register(v, m.id + ' %exports% ' + keys[i]);
    }
  }

  function execute(m) {
    for (var i = 0; i < m.depIds.length; i++) {
      var d = modules[m.depIds[i]];
      if (d) d.importers.delete(m.id);
    }
    m.depIds = [];
    var hot = {
      data: m.data,
      _dispose: [],
      accept: function (deps) {
        if (deps === undefined || typeof deps === 'function') m.accepts = true;
      },
      dispose: function (cb) {
        hot._dispose.push(cb);
      },
      decline: function () {
        hot._declined = true;
      },
      invalidate: function () {
        reload(m.id + ' called module.hot.invalidate()');
      },
    };
    var module = { id: m.id, exports: {}, hot: hot };
    m.module = module;
    m.hot = hot;
    m.accepts = false;
    m.loaded = true;  // before running: import cycles see the partial exports
    var reg = function (type, name) {
      if (Refresh) Refresh.register(type, m.id + ' ' + name);
    };
    var sig = Refresh ? Refresh.createSignatureFunctionForTransform : function () {
      return function (t) { return t; };
    };
    try {
      m.factory.call(module.exports, requireFrom(m), module, module.exports, reg, sig);
    } catch (e) {
      m.loaded = false;
      m.failed = true;
      throw e;
    }
    m.failed = false;
    m.boundary = !!Refresh && isBoundary(module.exports);
    m.signature = signatureOf(module.exports);
    if (m.boundary) registerExports(m);
  }

  // Modules to re-execute for a change to `ids`: each loaded one and its
  // importers, up to (and including) the nearest boundaries. A string is the
  // reason a full reload is needed instead.
  function plan(ids) {
    var queue = ids.slice();
    var seen = Object.create(null);
    var out = [];
    while (queue.length) {
      var id = queue.shift();
      if (seen[id]) continue;
      seen[id] = true;
      var m = modules[id];
      if (!m || (!m.loaded && !m.failed)) continue;  // not used yet: the new code runs when it is required
      if (m.hot && m.hot._declined) return m.id + ' declines hot updates';
      out.push(m);
      if (m.boundary || m.accepts) continue;
      if (m.importers.size === 0) {
        return m.id + (m.id === entryId ? ' (the entry module)' : '') +
          ' changed, and no module above it exports only React components';
      }
      m.importers.forEach(function (i) { queue.push(i); });
    }
    return out;
  }

  function apply(ids, to) {
    updates++;
    var failed = [];
    for (var id in modules) if (modules[id].failed) failed.push(id);
    var pending = failed.concat(ids);
    // A boundary whose export names change is re-run, then its importers
    // are too (they may use the new exports); a few rounds at most.
    for (var round = 0; pending.length && round < 8; round++) {
      var run = plan(pending);
      if (typeof run === 'string') return reload(run);
      pending = [];
      var before = Object.create(null);
      run.forEach(function (m) {
        before[m.id] = { boundary: m.boundary, signature: m.signature };
        var data = {};
        if (m.hot) {
          m.hot._dispose.forEach(function (cb) {
            try {
              cb(data);
            } catch (e) {
              log('error', 'module.hot.dispose handler of ' + m.id + ' threw: ' + describe(e));
            }
          });
        }
        m.data = data;
        m.loaded = false;
      });
      try {
        run.forEach(function (m) {
          if (!m.loaded) execute(m);
        });
      } catch (e) {
        // The next update re-runs everything this one did not finish.
        run.forEach(function (m) {
          if (!m.loaded) m.failed = true;
        });
        version = to;
        showError('Hot update failed (fix the code and save again):\n' + describe(e));
        return;
      }
      for (var i = 0; i < run.length; i++) {
        var m = run[i];
        var b = before[m.id];
        if (m.accepts || !b.boundary) continue;
        if (!m.boundary) return reload(m.id + ' no longer exports only React components');
        if (b.signature !== m.signature) m.importers.forEach(function (i) { pending.push(i); });
      }
    }
    version = to;
    clearError();
    if (Refresh) {
      try {
        Refresh.performReactRefresh();
      } catch (e) {
        showError(describe(e));
      }
    }
  }

  function buildError(text) {
    showError('Build failed:\n' + text);
  }

  // The update script's URL, as debuggers list it: <origin>/__jsui/hmr/<version>.js
  // (unique per update; its inline source map points at the app's files).
  var names = Object.create(null);
  function updateName(v) {
    var base = '/__jsui/hmr/' + encodeURIComponent(v || 'update');
    var name = base + '.js';
    for (var n = 2; names[name]; n++) name = base + '-' + n + '.js';
    names[name] = true;
    try {
      return new URL(name, G.location.href).href;
    } catch (e) {
      return name;
    }
  }

  function evalUpdate(code, v) {
    var name = updateName(v);
    if (native.evalScript) native.evalScript(code, name);
    else (0, eval)(code);
  }

  var busy = false;
  var wanted = false;
  function pump() {
    if (busy) return;
    busy = true;
    wanted = false;
    var url = new URL('/__jsui/hmr?from=' + encodeURIComponent(version || ''), G.location.href).href;
    G.fetch(url, { cache: 'no-store' })
      .then(function (res) {
        if (!res.ok) throw new Error('GET ' + url + ': status ' + res.status);
        var v = res.headers && res.headers.get ? res.headers.get('X-Jsui-Version') : null;
        return res.text().then(function (code) {
          return { code: code, version: v };
        });
      })
      .then(function (r) {
        busy = false;
        evalUpdate(r.code, r.version);
        if (wanted) pump();
      })
      .catch(function (e) {
        busy = false;
        reload('hot update failed: ' + describe(e));
      });
  }

  function start(id, v) {
    entryId = id;
    version = v;
    var m = modules[id];
    if (!m) throw new Error('hmr: no entry module ' + id);
    execute(m);
  }

  G.__jsui_onDevUpdate = function () {
    wanted = true;
    pump();
    return true;
  };

  G.__jsui_hmr = {
    define: define,
    start: start,
    apply: apply,
    reload: reload,
    buildError: buildError,
    get version() { return version; },
    get modules() { return modules; },
  };
})();
