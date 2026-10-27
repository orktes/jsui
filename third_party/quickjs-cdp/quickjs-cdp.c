/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in this directory.
 *
 * Vendored from react-native-quickjs (modules/cdp, commit c8102d4) with JSUI
 * changes marked "jsui:" (see README.md).
 *
 * The Chrome DevTools Protocol, for QuickJS. See quickjs-cdp.h.
 *
 * Protocol messages are JSON and the engine already parses JSON, so there is
 * no JSON library here and no generated marshalling code: a message is a
 * JSValue on a private context, and a reply is built by setting properties on
 * another one.
 */

#include "quickjs-cdp.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* jsui: a mutex and condition variable on Windows too (SRW locks there,
   pthreads elsewhere). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK cdp_mutex;
typedef CONDITION_VARIABLE cdp_cond;
#define cdp_mutex_init(m) InitializeSRWLock(m)
#define cdp_mutex_destroy(m) ((void)(m))
#define cdp_mutex_lock(m) AcquireSRWLockExclusive(m)
#define cdp_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#define cdp_cond_init(c) InitializeConditionVariable(c)
#define cdp_cond_destroy(c) ((void)(c))
#define cdp_cond_wait(c, m) SleepConditionVariableSRW(c, m, INFINITE, 0)
#define cdp_cond_broadcast(c) WakeAllConditionVariable(c)
#define strdup _strdup
#else
#include <pthread.h>
typedef pthread_mutex_t cdp_mutex;
typedef pthread_cond_t cdp_cond;
#define cdp_mutex_init(m) pthread_mutex_init(m, NULL)
#define cdp_mutex_destroy(m) pthread_mutex_destroy(m)
#define cdp_mutex_lock(m) pthread_mutex_lock(m)
#define cdp_mutex_unlock(m) pthread_mutex_unlock(m)
#define cdp_cond_init(c) pthread_cond_init(c, NULL)
#define cdp_cond_destroy(c) pthread_cond_destroy(c)
#define cdp_cond_wait(c, m) pthread_cond_wait(c, m)
#define cdp_cond_broadcast(c) pthread_cond_broadcast(c)
#endif

/* jsui: microseconds on a monotonic clock, for the profiler. */
#ifdef _WIN32
static int64_t cdp_now_us(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (int64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}
#else
#include <time.h>
static int64_t cdp_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

/* jsui: a node of the profiler's call tree. Functions are told apart by file,
   name and where they are defined. Nodes 0-2 are (root), (program), (idle). */
typedef struct {
  JSAtom file, func; /* dup'd atoms of the debuggee; JS_ATOM_NULL for 0-2 */
  int line, col;     /* 1-based definition, or -1 */
  int parent, first_child, next_sibling; /* indices, -1 for none */
  char *label;       /* a span's name ("(layout)"); NULL for functions */
} ProfNode;

typedef struct {
  int node;
  int64_t start, end;
} ProfSpan;

typedef struct {
  int id;
  char *url;
  char *source;
  char *source_map_url; /* jsui: from a trailing sourceMappingURL comment */
  bool has_source_url;
} Script;

typedef struct {
  int id;
  char *url;       /* matches a script URL ending with it; NULL with url_regex */
  char *url_regex; /* jsui: setBreakpointByUrl's urlRegex */
  char *condition;
  int line;
  int col;
  bool resolved;
} Breakpoint;

typedef struct {
  int id;
  JSValue value;
  char *group;
} RemoteObject;

typedef enum {
  MODE_RUN,
  MODE_PAUSE_NEXT,
  MODE_STEP_OVER,
  MODE_STEP_INTO,
  MODE_STEP_OUT
} PauseMode;

struct QJSCDPAgent {
  JSContext *ctx;

  /* Protocol JSON is parsed and printed on a runtime of the agent's own, so
     that handling a message cannot allocate in, collect from or throw into the
     heap the debugger exists to observe. */
  JSRuntime *jrt;
  JSContext *jctx;

  int exec_ctx_id;
  QJSCDPSendFunc *send;
  void *send_opaque;

  /* Messages arrive on the inspector thread and are handled on the JS thread.
     The condition variable is what lets the pause loop sleep rather than spin. */
  cdp_mutex lock;
  cdp_cond cond;
  char **queue;
  void **queue_session;
  int queue_len, queue_cap;

  /* The session whose request is being handled right now. Replies go to it;
     events go to everyone. */
  void *session;

  Script *scripts;
  int nscripts;
  Breakpoint *bps;
  int nbps, next_bp_id;
  RemoteObject *objs;
  int nobjs, next_obj_id;

  bool debugger_enabled;
  bool breakpoints_active;
  bool skip_all_pauses; /* jsui: Debugger.setSkipAllPauses */

  PauseMode mode;
  int step_depth;
  /* jsui: the breakpoint the last trap stopped at (or matched while
     suppressed), and its stack depth: the other statements on that line in
     the same call do not stop again. */
  int last_bp_id;
  int last_bp_depth;
  bool evaluating; /* jsui: a breakpoint condition is running */
  bool paused;
  bool resumed;
  volatile bool pause_pending; /* jsui: set from any thread */

  /* jsui: Profiler. sample_pending is set from the embedder's sampling
     thread; everything else is the JS thread's. */
  bool profiling;
  volatile bool sample_pending;
  int sampling_interval_us;
  ProfNode *pnodes;
  int npnodes, pnodes_cap;
  int *samples;
  int64_t *sample_times;
  int nsamples, samples_cap;
  int64_t profile_start;
  QJSCDPClockFunc *clock;
  ProfSpan *spans;
  int nspans, spans_cap;
  JSDebugFrameInfo *frames; /* a sample's stack */
  int frames_cap;
};

/* ---------------------------------------------------------------- JSON --- */

static JSValue json_object(QJSCDPAgent *agent) {
  return JS_NewObject(agent->jctx);
}

static JSValue json_array(QJSCDPAgent *agent) {
  return JS_NewArray(agent->jctx);
}

static void json_set(QJSCDPAgent *agent, JSValue o, const char *k, JSValue v) {
  JS_SetPropertyStr(agent->jctx, o, k, v);
}

static void json_set_string(
    QJSCDPAgent *agent, JSValue o, const char *k, const char *v) {
  json_set(agent, o, k, JS_NewString(agent->jctx, v ? v : ""));
}

static void json_set_int(
    QJSCDPAgent *agent, JSValue o, const char *k, int64_t v) {
  json_set(agent, o, k, JS_NewInt64(agent->jctx, v));
}

static void json_set_bool(
    QJSCDPAgent *agent, JSValue o, const char *k, bool v) {
  json_set(agent, o, k, JS_NewBool(agent->jctx, v));
}

static void json_set_index(
    QJSCDPAgent *agent, JSValue arr, uint32_t i, JSValue v) {
  JS_SetPropertyUint32(agent->jctx, arr, i, v);
}

static JSValue json_get(QJSCDPAgent *agent, JSValue o, const char *k) {
  return JS_GetPropertyStr(agent->jctx, o, k);
}

/* Result must be released with JS_FreeCString(agent->jctx, ...). NULL when the
   property is absent or null. */
static const char *json_get_string(
    QJSCDPAgent *agent, JSValue o, const char *k) {
  JSValue v = json_get(agent, o, k);
  if (JS_IsUndefined(v) || JS_IsNull(v) || JS_IsException(v)) {
    JS_FreeValue(agent->jctx, v);
    return NULL;
  }
  const char *s = JS_ToCString(agent->jctx, v);
  JS_FreeValue(agent->jctx, v);
  return s;
}

static int json_get_int(
    QJSCDPAgent *agent, JSValue o, const char *k, int fallback) {
  JSValue v = json_get(agent, o, k);
  int32_t n = fallback;
  if (JS_IsNumber(v)) JS_ToInt32(agent->jctx, &n, v);
  JS_FreeValue(agent->jctx, v);
  return n;
}

static bool json_get_bool(
    QJSCDPAgent *agent, JSValue o, const char *k, bool fallback) {
  JSValue v = json_get(agent, o, k);
  bool b = JS_IsBool(v) ? JS_ToBool(agent->jctx, v) : fallback;
  JS_FreeValue(agent->jctx, v);
  return b;
}

/* Takes ownership of msg. */
static void send_message(QJSCDPAgent *agent, JSValue msg) {
  JSValue s = JS_JSONStringify(agent->jctx, msg, JS_UNDEFINED, JS_UNDEFINED);
  size_t len = 0;
  const char *text = JS_ToCStringLen(agent->jctx, &len, s);
  if (text) {
    agent->send(agent->send_opaque, agent->session, text, len);
    JS_FreeCString(agent->jctx, text);
  }
  JS_FreeValue(agent->jctx, s);
  JS_FreeValue(agent->jctx, msg);
}

/* Takes ownership of result. */
static void send_result(QJSCDPAgent *agent, int id, JSValue result) {
  JSValue m = json_object(agent);
  json_set_int(agent, m, "id", id);
  json_set(agent, m, "result", result);
  send_message(agent, m);
}

static void send_empty_result(QJSCDPAgent *agent, int id) {
  send_result(agent, id, json_object(agent));
}

static void send_error_code(
    QJSCDPAgent *agent, int id, int code, const char *message) {
  JSValue e = json_object(agent);
  json_set_int(agent, e, "code", code);
  json_set_string(agent, e, "message", message);
  JSValue m = json_object(agent);
  json_set_int(agent, m, "id", id);
  json_set(agent, m, "error", e);
  send_message(agent, m);
}

/* -32000 is "server error", which is what the protocol uses for anything the
   engine simply could not do. */
static void send_error(QJSCDPAgent *agent, int id, const char *message) {
  send_error_code(agent, id, -32000, message);
}

/* Takes ownership of params. `everyone` distinguishes something that happened
   in the program, which every attached frontend wants, from a reply-shaped
   event such as the scripts replayed when one frontend enables the debugger --
   which the others have already been told about. */
static void send_event_to(
    QJSCDPAgent *agent, const char *method, JSValue params, bool everyone) {
  void *asked = agent->session;
  if (everyone) agent->session = NULL;
  JSValue m = json_object(agent);
  json_set_string(agent, m, "method", method);
  json_set(agent, m, "params", params);
  send_message(agent, m);
  agent->session = asked;
}

static void send_event(QJSCDPAgent *agent, const char *method, JSValue params) {
  send_event_to(agent, method, params, true);
}

/* ------------------------------------------------------- remote objects --- */

static int remember_object(QJSCDPAgent *agent, JSValue v, const char *group) {
  agent->objs = realloc(agent->objs, sizeof(RemoteObject) * (agent->nobjs + 1));
  RemoteObject *r = &agent->objs[agent->nobjs++];
  r->id = ++agent->next_obj_id;
  r->value = JS_DupValue(agent->ctx, v);
  r->group = group ? strdup(group) : NULL;
  return r->id;
}

static RemoteObject *find_object(QJSCDPAgent *agent, const char *object_id) {
  if (!object_id) return NULL;
  int id = atoi(object_id);
  for (int i = 0; i < agent->nobjs; i++)
    if (agent->objs[i].id == id) return &agent->objs[i];
  return NULL;
}

static void forget_object(QJSCDPAgent *agent, int i) {
  JS_FreeValue(agent->ctx, agent->objs[i].value);
  free(agent->objs[i].group);
  agent->objs[i] = agent->objs[--agent->nobjs];
}

static bool release_object(QJSCDPAgent *agent, const char *object_id) {
  RemoteObject *r = find_object(agent, object_id);
  if (!r) return false;
  forget_object(agent, (int)(r - agent->objs));
  return true;
}

static void release_object_group(QJSCDPAgent *agent, const char *group) {
  if (!group) return;
  for (int i = agent->nobjs - 1; i >= 0; i--)
    if (agent->objs[i].group && strcmp(agent->objs[i].group, group) == 0)
      forget_object(agent, i);
}

/* Debuggee -> C string, swallowing any exception a toString() raises. The
   frontend asked for a description, not for the program's error state. */
static const char *debuggee_to_string(QJSCDPAgent *agent, JSValue v) {
  const char *s = JS_ToCString(agent->ctx, v);
  if (!s && JS_HasException(agent->ctx))
    JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
  return s;
}

static void free_debuggee_string(QJSCDPAgent *agent, const char *s) {
  JS_FreeCString(agent->ctx, s);
}

static const char *subtype_name(QJSCDPAgent *agent, JSValue v) {
  if (JS_IsNull(v)) return "null";
  if (JS_IsArray(v)) return "array";
  if (JS_IsError(v)) return "error";
  if (JS_IsDate(v)) return "date";
  if (JS_IsRegExp(v)) return "regexp";
  if (JS_IsMap(v)) return "map";
  if (JS_IsPromise(v)) return "promise";
  if (JS_IsProxy(v)) return "proxy";
  return NULL;
}

static void set_description(QJSCDPAgent *agent, JSValue out, JSValue v) {
  const char *s = debuggee_to_string(agent, v);
  if (s) {
    json_set_string(agent, out, "description", s);
    free_debuggee_string(agent, s);
  }
}

/* Takes a borrowed debuggee value; returns a protocol-context object. */
static JSValue to_remote_object(
    QJSCDPAgent *agent, JSValue v, const char *group) {
  JSValue o = json_object(agent);

  if (JS_IsUndefined(v)) {
    json_set_string(agent, o, "type", "undefined");
    return o;
  }
  if (JS_IsNull(v)) {
    json_set_string(agent, o, "type", "object");
    json_set_string(agent, o, "subtype", "null");
    json_set(agent, o, "value", JS_NULL);
    return o;
  }
  if (JS_IsBool(v)) {
    json_set_string(agent, o, "type", "boolean");
    json_set_bool(agent, o, "value", JS_ToBool(agent->ctx, v));
    return o;
  }
  if (JS_IsNumber(v)) {
    double d;
    JS_ToFloat64(agent->ctx, &d, v);
    json_set_string(agent, o, "type", "number");
    /* NaN, the infinities and -0 have no JSON spelling, so the protocol sends
       them by name instead of as a value. Every other number is just a
       number: adding a description to those would be noise. */
    const char *name = NULL;
    if (isnan(d))
      name = "NaN";
    else if (isinf(d))
      name = d > 0 ? "Infinity" : "-Infinity";
    else if (d == 0 && signbit(d))
      name = "-0";
    if (name) {
      json_set_string(agent, o, "unserializableValue", name);
      json_set_string(agent, o, "description", name);
    } else {
      json_set(agent, o, "value", JS_NewFloat64(agent->jctx, d));
    }
    return o;
  }
  if (JS_IsString(v)) {
    const char *s = debuggee_to_string(agent, v);
    json_set_string(agent, o, "type", "string");
    json_set_string(agent, o, "value", s);
    free_debuggee_string(agent, s);
    return o;
  }
  if (JS_IsBigInt(v)) {
    const char *s = debuggee_to_string(agent, v);
    json_set_string(agent, o, "type", "bigint");
    json_set_string(agent, o, "description", s);
    free_debuggee_string(agent, s);
    return o;
  }
  if (JS_IsSymbol(v)) {
    const char *s = debuggee_to_string(agent, v);
    json_set_string(agent, o, "type", "symbol");
    json_set_string(agent, o, "description", s);
    free_debuggee_string(agent, s);
    char sid[16];
    snprintf(sid, sizeof(sid), "%d", remember_object(agent, v, group));
    json_set_string(agent, o, "objectId", sid);
    return o;
  }

  bool is_fn = JS_IsFunction(agent->ctx, v);
  json_set_string(agent, o, "type", is_fn ? "function" : "object");

  const char *sub = subtype_name(agent, v);
  if (sub) json_set_string(agent, o, "subtype", sub);

  /* className comes from the constructor rather than from toString, which for
     a plain object is "[object Object]" and tells the frontend nothing. */
  JSValue ctor = JS_GetPropertyStr(agent->ctx, v, "constructor");
  JSValue name = JS_IsObject(ctor) ? JS_GetPropertyStr(agent->ctx, ctor, "name")
                                   : JS_UNDEFINED;
  const char *cn = JS_IsString(name) ? debuggee_to_string(agent, name) : NULL;
  json_set_string(agent, o, "className", cn && *cn ? cn : "Object");

  /* jsui: describe objects the way V8 does ("Object", "Array(3)"), not as
     "[object Object]"; functions, errors, dates and regexps by their text. */
  if (is_fn || (sub && (!strcmp(sub, "error") || !strcmp(sub, "date") ||
                        !strcmp(sub, "regexp")))) {
    set_description(agent, o, v);
  } else if (sub && !strcmp(sub, "array")) {
    int64_t len = 0;
    if (JS_GetLength(agent->ctx, v, &len) < 0)
      JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
    char d[64];
    snprintf(d, sizeof(d), "Array(%lld)", (long long)len);
    json_set_string(agent, o, "description", d);
  } else {
    json_set_string(agent, o, "description", cn && *cn ? cn : "Object");
  }
  if (cn) free_debuggee_string(agent, cn);
  JS_FreeValue(agent->ctx, name);
  JS_FreeValue(agent->ctx, ctor);

  char id[16];
  snprintf(id, sizeof(id), "%d", remember_object(agent, v, group));
  json_set_string(agent, o, "objectId", id);
  return o;
}

/* Takes ownership of exc. */
static JSValue to_exception_details(QJSCDPAgent *agent, JSValue exc) {
  JSValue d = json_object(agent);
  json_set_int(agent, d, "exceptionId", 1);
  json_set_int(agent, d, "lineNumber", 0);
  json_set_int(agent, d, "columnNumber", 0);

  const char *text = debuggee_to_string(agent, exc);
  json_set_string(agent, d, "text", text ? text : "Uncaught");
  if (text) free_debuggee_string(agent, text);

  json_set(agent, d, "exception", to_remote_object(agent, exc, NULL));
  JS_FreeValue(agent->ctx, exc);
  return d;
}

/* ------------------------------------------------------------- scripts --- */

static Script *find_script_by_url(QJSCDPAgent *agent, const char *url) {
  if (!url) return NULL;
  for (int i = 0; i < agent->nscripts; i++)
    if (strcmp(agent->scripts[i].url, url) == 0) return &agent->scripts[i];
  return NULL;
}

static Script *find_script_by_id(QJSCDPAgent *agent, const char *script_id) {
  if (!script_id) return NULL;
  int id = atoi(script_id);
  for (int i = 0; i < agent->nscripts; i++)
    if (agent->scripts[i].id == id) return &agent->scripts[i];
  return NULL;
}

/* A breakpoint URL matches a script whose URL ends with it, so that a frontend
   that only knows "app.js" still binds against "http://localhost/app.js". */
static bool url_matches(const char *script_url, const char *wanted) {
  size_t n = strlen(script_url), m = strlen(wanted);
  return m <= n && strcmp(script_url + n - m, wanted) == 0;
}

static void announce_script(
    QJSCDPAgent *agent, const Script *script, bool everyone) {
  char id[16];
  snprintf(id, sizeof(id), "%d", script->id);

  int lines = 0;
  for (const char *p = script->source; *p; p++)
    if (*p == '\n') lines++;

  JSValue p = json_object(agent);
  json_set_string(agent, p, "scriptId", id);
  json_set_string(agent, p, "url", script->url);
  json_set_int(agent, p, "startLine", 0);
  json_set_int(agent, p, "startColumn", 0);
  json_set_int(agent, p, "endLine", lines);
  json_set_int(agent, p, "endColumn", 0);
  json_set_int(agent, p, "executionContextId", agent->exec_ctx_id);
  json_set_string(agent, p, "hash", id);
  if (script->source_map_url)
    json_set_string(agent, p, "sourceMapURL", script->source_map_url);
  if (script->has_source_url) json_set_bool(agent, p, "hasSourceURL", true);
  send_event_to(agent, "Debugger.scriptParsed", p, everyone);
}

/* --------------------------------------------------------- breakpoints --- */

static void announce_breakpoint(
    QJSCDPAgent *agent, const Breakpoint *bp, const Script *script) {
  char bid[16], sid[16];
  snprintf(bid, sizeof(bid), "%d", bp->id);
  snprintf(sid, sizeof(sid), "%d", script->id);

  JSValue loc = json_object(agent);
  json_set_string(agent, loc, "scriptId", sid);
  json_set_int(agent, loc, "lineNumber", bp->line - 1);
  json_set_int(agent, loc, "columnNumber", bp->col > 0 ? bp->col - 1 : 0);

  JSValue p = json_object(agent);
  json_set_string(agent, p, "breakpointId", bid);
  json_set(agent, p, "location", loc);
  send_event(agent, "Debugger.breakpointResolved", p);
}

/* Binds every breakpoint that named this script's URL but had no script to
   attach to when it was set. */
/* jsui: whether `url` matches the JavaScript regular expression `pattern`
   (run on the agent's own context, never the debuggee's). */
static bool regex_matches(QJSCDPAgent *agent, const char *pattern,
                          const char *url) {
  JSContext *c = agent->jctx;
  JSValue global = JS_GetGlobalObject(c);
  JSValue ctor = JS_GetPropertyStr(c, global, "RegExp");
  JSValue pat = JS_NewString(c, pattern);
  JSValue re = JS_CallConstructor(c, ctor, 1, &pat);
  bool ok = false;
  if (!JS_IsException(re)) {
    JSValue test = JS_GetPropertyStr(c, re, "test");
    JSValue arg = JS_NewString(c, url);
    JSValue r = JS_Call(c, test, re, 1, &arg);
    ok = JS_ToBool(c, r) > 0;
    JS_FreeValue(c, r);
    JS_FreeValue(c, arg);
    JS_FreeValue(c, test);
  }
  if (JS_HasException(c)) JS_FreeValue(c, JS_GetException(c));
  JS_FreeValue(c, re);
  JS_FreeValue(c, pat);
  JS_FreeValue(c, ctor);
  JS_FreeValue(c, global);
  return ok;
}

static bool bp_matches(QJSCDPAgent *agent, const Breakpoint *bp,
                       const char *script_url) {
  if (bp->url) return url_matches(script_url, bp->url);
  return bp->url_regex && regex_matches(agent, bp->url_regex, script_url);
}

static void resolve_breakpoints(QJSCDPAgent *agent, const Script *script) {
  for (int i = 0; i < agent->nbps; i++) {
    Breakpoint *bp = &agent->bps[i];
    if (bp->resolved || !bp_matches(agent, bp, script->url)) continue;
    bp->resolved = true;
    announce_breakpoint(agent, bp, script);
  }
}

static Breakpoint *breakpoint_at(
    QJSCDPAgent *agent, const char *file, int line) {
  if (!agent->breakpoints_active || !file) return NULL;
  for (int i = 0; i < agent->nbps; i++)
    if (agent->bps[i].line == line && bp_matches(agent, &agent->bps[i], file))
      return &agent->bps[i];
  return NULL;
}

/* jsui: a cheap first test for the per-statement trap: does any breakpoint
   sit on this line at all? */
static bool any_breakpoint_on_line(QJSCDPAgent *agent, int line) {
  if (!agent->breakpoints_active) return false;
  for (int i = 0; i < agent->nbps; i++)
    if (agent->bps[i].line == line) return true;
  return false;
}

/* --------------------------------------------------------- call frames --- */

/* The frame's arguments, locals and captured variables, as one debuggee object
   the frontend can expand. */
static JSValue frame_scope_object(QJSCDPAgent *agent, int level) {
  JSDebugLocalVar *vars = NULL;
  int count = 0;
  if (JS_GetLocalVariablesAtLevel(agent->ctx, level, &vars, &count) < 0)
    return JS_NewObject(agent->ctx);

  JSValue scope = JS_NewObject(agent->ctx);
  for (int i = 0; i < count; i++)
    /* jsui: not the engine's hidden temporaries (a destructured parameter
       is a variable named "\x01"). */
    if (vars[i].name && (unsigned char)vars[i].name[0] >= ' ')
      JS_SetPropertyStr(
        agent->ctx, scope, vars[i].name,
        JS_DupValue(agent->ctx, vars[i].value));
  JS_FreeLocalVariables(agent->ctx, vars, count);
  return scope;
}

/* quickjs names the top-level frame "<eval>"; the protocol calls it "global". */
static const char *frame_name(const char *func) {
  if (!func || !*func) return "";
  return strcmp(func, "<eval>") == 0 ? "global" : func;
}

static JSValue call_frames(QJSCDPAgent *agent) {
  JSValue frames = json_array(agent);
  int depth = JS_GetStackDepth(agent->ctx);
  uint32_t n = 0;

  for (int level = 0; level < depth; level++) {
    JSDebugFrameInfo info;
    if (JS_GetFrameInfoAtLevel(agent->ctx, level, &info) < 0) break;
    if (info.is_native) continue;

    const char *file = info.filename != JS_ATOM_NULL
                           ? JS_AtomToCString(agent->ctx, info.filename)
                           : NULL;
    const char *func = info.func_name != JS_ATOM_NULL
                           ? JS_AtomToCString(agent->ctx, info.func_name)
                           : NULL;
    Script *script = file ? find_script_by_url(agent, file) : NULL;
    /* jsui: frames of code the frontend was never told about (the runtime's
       own prelude) would name a script it cannot show. */
    if (!script) {
      if (file) JS_FreeCString(agent->ctx, file);
      if (func) JS_FreeCString(agent->ctx, func);
      continue;
    }

    char frame_id[16], script_id[16];
    snprintf(frame_id, sizeof(frame_id), "%d", level);
    snprintf(script_id, sizeof(script_id), "%d", script ? script->id : 0);

    /* CDP counts lines and columns from zero; the engine counts from one. */
    JSValue location = json_object(agent);
    json_set_string(agent, location, "scriptId", script_id);
    json_set_int(
        agent, location, "lineNumber", info.line > 0 ? info.line - 1 : 0);
    json_set_int(
        agent, location, "columnNumber", info.col > 0 ? info.col - 1 : 0);

    JSValue locals = frame_scope_object(agent, level);
    JSValue scope = json_object(agent);
    json_set_string(agent, scope, "type", "local");
    json_set(
        agent, scope, "object", to_remote_object(agent, locals, "backtrace"));
    JS_FreeValue(agent->ctx, locals);

    JSValue scope_chain = json_array(agent);
    json_set_index(agent, scope_chain, 0, scope);
    /* jsui: and the global scope below it. */
    JSValue global = JS_GetGlobalObject(agent->ctx);
    JSValue gscope = json_object(agent);
    json_set_string(agent, gscope, "type", "global");
    json_set(agent, gscope, "object",
             to_remote_object(agent, global, "backtrace"));
    json_set_index(agent, scope_chain, 1, gscope);
    JS_FreeValue(agent->ctx, global);

    JSValue frame = json_object(agent);
    json_set_string(agent, frame, "callFrameId", frame_id);
    json_set_string(agent, frame, "functionName", frame_name(func));
    json_set(agent, frame, "location", location);
    json_set(agent, frame, "url", JS_NewString(agent->jctx, file ? file : ""));
    json_set(agent, frame, "scopeChain", scope_chain);
    json_set(agent, frame, "this", to_remote_object(agent, JS_UNDEFINED, NULL));
    json_set_index(agent, frames, n++, frame);

    if (file) JS_FreeCString(agent->ctx, file);
    if (func) JS_FreeCString(agent->ctx, func);
  }
  return frames;
}

/* ------------------------------------------------------------- pausing --- */

static void handle_message(QJSCDPAgent *agent, const char *json, size_t len);

/* Caller holds the lock. */
/*
 * The agent's own runtime is entered from whichever thread is currently
 * driving the agent, and the engine records the stack bounds of the thread
 * that created a runtime. React Native creates the agent on one thread and
 * then calls it from the JS thread; the pause loop runs on the JS thread while
 * the frontend posts from another.
 *
 * Without this, every use of the agent's runtime from a second thread fails
 * the stack check on its first allocation -- so a posted Debugger.resume never
 * parses, the pause loop never wakes, and the app hangs at the breakpoint.
 * Both thread stacks happen to sit close together on macOS, so it only shows
 * up on Linux and Android.
 */
static void enter_agent_runtime(QJSCDPAgent *agent) {
  JS_UpdateStackTop(agent->jrt);
}

static char *take_message(QJSCDPAgent *agent, void **session) {
  if (agent->queue_len == 0) return NULL;
  char *msg = agent->queue[0];
  *session = agent->queue_session[0];
  agent->queue_len--;
  memmove(
      agent->queue, agent->queue + 1,
      sizeof(char *) * (size_t)agent->queue_len);
  memmove(
      agent->queue_session, agent->queue_session + 1,
      sizeof(void *) * (size_t)agent->queue_len);
  return msg;
}

/* jsui: the statement traps only run while something could stop at one: a
   breakpoint, a step, a requested pause. An attached debugger with none of
   those costs nothing per statement (it was ~4-6x slower with them armed).
   `debugger;` statements reach the handler armed or not. JS thread only
   (disarming is); qjs_cdp_pause arms from any thread. */
static void update_arming(QJSCDPAgent *agent) {
  bool armed = (agent->debugger_enabled && !agent->skip_all_pauses &&
                (agent->pause_pending || agent->mode != MODE_RUN ||
                 (agent->breakpoints_active && agent->nbps > 0))) ||
               agent->sample_pending; /* jsui: a profiler sample is due */
  JS_SetDebugTraceArmed(agent->ctx, armed);
  /* A pause requested from another thread meanwhile must not be lost. */
  if (!armed && agent->pause_pending && agent->debugger_enabled)
    JS_SetDebugTraceArmed(agent->ctx, true);
}

/* ------------------------------------------------------ jsui: profiler --- */

static void profiler_reset(QJSCDPAgent *agent) {
  for (int i = 0; i < agent->npnodes; i++) {
    if (agent->pnodes[i].file != JS_ATOM_NULL)
      JS_FreeAtom(agent->ctx, agent->pnodes[i].file);
    if (agent->pnodes[i].func != JS_ATOM_NULL)
      JS_FreeAtom(agent->ctx, agent->pnodes[i].func);
    free(agent->pnodes[i].label);
  }
  agent->npnodes = 0;
  agent->nsamples = 0;
  agent->nspans = 0;
}

static int64_t profiler_now(QJSCDPAgent *agent) {
  return agent->clock ? agent->clock() : cdp_now_us();
}

static int profiler_node(
    QJSCDPAgent *agent, int parent, JSAtom file, JSAtom func, int line,
    int col) {
  if (parent >= 0) {
    for (int c = agent->pnodes[parent].first_child; c >= 0;
         c = agent->pnodes[c].next_sibling) {
      ProfNode *n = &agent->pnodes[c];
      if (n->file == file && n->func == func && n->line == line && n->col == col)
        return c;
    }
  }
  if (agent->npnodes == agent->pnodes_cap) {
    agent->pnodes_cap = agent->pnodes_cap ? agent->pnodes_cap * 2 : 256;
    agent->pnodes =
        realloc(agent->pnodes, sizeof(ProfNode) * (size_t)agent->pnodes_cap);
  }
  int id = agent->npnodes++;
  ProfNode *n = &agent->pnodes[id];
  n->file = file != JS_ATOM_NULL ? JS_DupAtom(agent->ctx, file) : JS_ATOM_NULL;
  n->func = func != JS_ATOM_NULL ? JS_DupAtom(agent->ctx, func) : JS_ATOM_NULL;
  n->line = line;
  n->col = col;
  n->parent = parent;
  n->first_child = -1;
  n->next_sibling = -1;
  n->label = NULL;
  if (parent >= 0) {
    n->next_sibling = agent->pnodes[parent].first_child;
    agent->pnodes[parent].first_child = id;
  }
  return id;
}

enum { PROF_ROOT = 0, PROF_PROGRAM = 1, PROF_IDLE = 2 };

static void profiler_push_at(QJSCDPAgent *agent, int node, int64_t t) {
  if (agent->nsamples == agent->samples_cap) {
    agent->samples_cap = agent->samples_cap ? agent->samples_cap * 2 : 4096;
    agent->samples =
        realloc(agent->samples, sizeof(int) * (size_t)agent->samples_cap);
    agent->sample_times = realloc(
        agent->sample_times, sizeof(int64_t) * (size_t)agent->samples_cap);
  }
  agent->samples[agent->nsamples] = node;
  agent->sample_times[agent->nsamples] = t;
  agent->nsamples++;
}

static void profiler_push(QJSCDPAgent *agent, int node) {
  profiler_push_at(agent, node, profiler_now(agent));
}

static void profiler_start(QJSCDPAgent *agent) {
  profiler_reset(agent);
  profiler_node(agent, -1, JS_ATOM_NULL, JS_ATOM_NULL, -1, -1);
  profiler_node(agent, PROF_ROOT, JS_ATOM_NULL, JS_ATOM_NULL, -1, -1);
  profiler_node(agent, PROF_ROOT, JS_ATOM_NULL, JS_ATOM_NULL, -2, -2);
  agent->profile_start = profiler_now(agent);
  agent->profiling = true;
  profiler_push(agent, PROF_IDLE); /* messages are handled while JS is idle */
}

/* The JS stack now, outermost frame first, as a path in the call tree. The
   time until the next sample is spent here (in the innermost frame, or in a
   native function it called). */
static void profiler_sample(QJSCDPAgent *agent) {
  int depth = JS_GetStackDepth(agent->ctx);
  if (depth > agent->frames_cap) {
    agent->frames_cap = depth * 2;
    agent->frames = realloc(
        agent->frames, sizeof(JSDebugFrameInfo) * (size_t)agent->frames_cap);
  }
  int n = JS_GetStackFunctions(agent->ctx, agent->frames, agent->frames_cap);
  int node = PROF_ROOT;
  for (int level = n - 1; level >= 0; level--) {
    JSDebugFrameInfo *info = &agent->frames[level];
    if (info->is_native) continue;
    node = profiler_node(agent, node, info->filename, info->func_name,
                         info->func_line, info->func_col);
  }
  profiler_push(agent, node == PROF_ROOT ? PROF_PROGRAM : node);
}

static int span_cmp(const void *a, const void *b) {
  int64_t x = ((const ProfSpan *)a)->start, y = ((const ProfSpan *)b)->start;
  return x < y ? -1 : x > y;
}

/* Each (idle) stretch of the samples becomes the spans that fall in it, with
   (idle) left for the rest. Spans that overlap JS are cut to its gaps. */
static void profiler_merge_spans(QJSCDPAgent *agent, int64_t end) {
  if (!agent->nspans) return;
  qsort(agent->spans, (size_t)agent->nspans, sizeof(ProfSpan), span_cmp);
  int n = agent->nsamples;
  int *old_samples = agent->samples;
  int64_t *old_times = agent->sample_times;
  agent->samples = NULL;
  agent->sample_times = NULL;
  agent->nsamples = agent->samples_cap = 0;
  int first = 0; /* spans before this one end before the current stretch */
  for (int i = 0; i < n; i++) {
    int64_t t = old_times[i], next = i + 1 < n ? old_times[i + 1] : end;
    if (old_samples[i] != PROF_IDLE) {
      profiler_push_at(agent, old_samples[i], t);
      continue;
    }
    int64_t cursor = t;
    while (first < agent->nspans && agent->spans[first].end <= t) first++;
    for (int k = first; k < agent->nspans && agent->spans[k].start < next; k++) {
      int64_t a = agent->spans[k].start > cursor ? agent->spans[k].start : cursor;
      int64_t b = agent->spans[k].end < next ? agent->spans[k].end : next;
      if (a >= b) continue;
      if (a > cursor) profiler_push_at(agent, PROF_IDLE, cursor);
      profiler_push_at(agent, agent->spans[k].node, a);
      cursor = b;
    }
    if (cursor < next || cursor == t) profiler_push_at(agent, PROF_IDLE, cursor);
  }
  free(old_samples);
  free(old_times);
}

static JSValue profiler_call_frame(QJSCDPAgent *agent, int i) {
  static const char *const special[] = {"(root)", "(program)", "(idle)"};
  ProfNode *n = &agent->pnodes[i];
  JSValue frame = json_object(agent);
  const char *file = n->file != JS_ATOM_NULL
                         ? JS_AtomToCString(agent->ctx, n->file)
                         : NULL;
  const char *func = n->func != JS_ATOM_NULL
                         ? JS_AtomToCString(agent->ctx, n->func)
                         : NULL;
  Script *script = file ? find_script_by_url(agent, file) : NULL;
  char script_id[16];
  snprintf(script_id, sizeof(script_id), "%d", script ? script->id : 0);
  json_set_string(agent, frame, "functionName",
                  i <= PROF_IDLE ? special[i]
                  : n->label     ? n->label
                                 : frame_name(func));
  json_set_string(agent, frame, "scriptId", script_id);
  json_set_string(agent, frame, "url", file ? file : "");
  json_set_int(agent, frame, "lineNumber", n->line > 0 ? n->line - 1 : -1);
  json_set_int(agent, frame, "columnNumber", n->col > 0 ? n->col - 1 : -1);
  if (file) JS_FreeCString(agent->ctx, file);
  if (func) JS_FreeCString(agent->ctx, func);
  return frame;
}

/* Profiler.Profile: the call tree, the node of every sample and the time
   between them. Ends the recording. */
static JSValue profiler_stop(QJSCDPAgent *agent) {
  agent->profiling = false;
  agent->sample_pending = false;
  update_arming(agent);
  int64_t end = profiler_now(agent);
  profiler_merge_spans(agent, end);

  int *hits = calloc((size_t)agent->npnodes, sizeof(int));
  for (int i = 0; i < agent->nsamples; i++) hits[agent->samples[i]]++;

  JSValue nodes = json_array(agent);
  for (int i = 0; i < agent->npnodes; i++) {
    JSValue node = json_object(agent);
    json_set_int(agent, node, "id", i + 1);
    json_set(agent, node, "callFrame", profiler_call_frame(agent, i));
    json_set_int(agent, node, "hitCount", hits[i]);
    JSValue children = json_array(agent);
    uint32_t nc = 0;
    for (int c = agent->pnodes[i].first_child; c >= 0;
         c = agent->pnodes[c].next_sibling)
      json_set_index(agent, children, nc++, JS_NewInt32(agent->jctx, c + 1));
    json_set(agent, node, "children", children);
    json_set_index(agent, nodes, (uint32_t)i, node);
  }
  free(hits);

  JSValue samples = json_array(agent);
  JSValue deltas = json_array(agent);
  int64_t prev = agent->profile_start;
  for (int i = 0; i < agent->nsamples; i++) {
    json_set_index(agent, samples, (uint32_t)i,
                   JS_NewInt32(agent->jctx, agent->samples[i] + 1));
    json_set_index(agent, deltas, (uint32_t)i,
                   JS_NewInt64(agent->jctx, agent->sample_times[i] - prev));
    prev = agent->sample_times[i];
  }

  JSValue profile = json_object(agent);
  json_set(agent, profile, "nodes", nodes);
  json_set(agent, profile, "startTime",
           JS_NewInt64(agent->jctx, agent->profile_start));
  json_set(agent, profile, "endTime", JS_NewInt64(agent->jctx, end));
  json_set(agent, profile, "samples", samples);
  json_set(agent, profile, "timeDeltas", deltas);

  /* The embedder's spans as they were, before they were cut to JS's gaps:
     [name, start, end] each, for a trace with a lane per thread (`jsui dev`
     writes one). DevTools ignores the field. */
  JSValue spans = json_array(agent);
  for (int i = 0; i < agent->nspans; i++) {
    JSValue sp = json_array(agent);
    json_set_index(agent, sp, 0,
                   JS_NewString(agent->jctx,
                                agent->pnodes[agent->spans[i].node].label));
    json_set_index(agent, sp, 1, JS_NewInt64(agent->jctx, agent->spans[i].start));
    json_set_index(agent, sp, 2, JS_NewInt64(agent->jctx, agent->spans[i].end));
    json_set_index(agent, spans, (uint32_t)i, sp);
  }
  json_set(agent, profile, "jsuiSpans", spans);
  profiler_reset(agent);
  return profile;
}

/* Stops the JS thread here and answers the frontend from inside the engine
   until it is told to continue. This is the only reason the message queue
   exists: while this loop is running the JS thread cannot return to whatever
   would otherwise be pumping it. */
static void pause_until_resumed(
    QJSCDPAgent *agent, const char *reason, int breakpoint_id) {
  agent->paused = true;
  agent->resumed = false;
  agent->pause_pending = false;
  agent->mode = MODE_RUN;

  JSValue params = json_object(agent);
  json_set(agent, params, "callFrames", call_frames(agent));
  json_set_string(agent, params, "reason", reason);
  if (breakpoint_id > 0) {
    char bid[16];
    snprintf(bid, sizeof(bid), "%d", breakpoint_id);
    JSValue hit = json_array(agent);
    json_set_index(agent, hit, 0, JS_NewString(agent->jctx, bid));
    json_set(agent, params, "hitBreakpoints", hit);
  }
  send_event(agent, "Debugger.paused", params);

  cdp_mutex_lock(&agent->lock);
  for (;;) {
    void *session = NULL;
    char *msg = take_message(agent, &session);
    if (msg) {
      cdp_mutex_unlock(&agent->lock);
      agent->session = session;
      handle_message(agent, msg, strlen(msg));
      agent->session = NULL;
      free(msg);
      cdp_mutex_lock(&agent->lock);
      continue;
    }
    if (agent->resumed) break;
    cdp_cond_wait(&agent->cond, &agent->lock);
  }
  cdp_mutex_unlock(&agent->lock);

  agent->paused = false;
  update_arming(agent); /* jsui: on for a step, off for a plain resume */
  /* jsui: the scopes of the frames shown while paused are not used again. */
  release_object_group(agent, "backtrace");
  send_event(agent, "Debugger.resumed", json_object(agent));
}

static int on_statement(
    JSContext *ctx, JSAtom filename, JSAtom funcname, int line, int col,
    int flags, void *opaque) {
  (void)funcname;
  (void)col;
  QJSCDPAgent *agent = opaque;
  enter_agent_runtime(agent);

  if (agent->sample_pending) { /* jsui: the profiler's tick armed this trap */
    agent->sample_pending = false;
    if (agent->profiling && !agent->paused) profiler_sample(agent);
    update_arming(agent);
  }

  /* Re-entrant: an evaluate issued from the pause loop runs instrumented code
     of its own, and stopping inside it would deadlock the frontend. */
  if (agent->paused || agent->evaluating || agent->skip_all_pauses ||
      !agent->debugger_enabled)
    return 0;

  const char *reason = NULL;
  int breakpoint_id = 0;
  int depth = JS_GetStackDepth(ctx);

  if (agent->pause_pending) {
    reason = "other";
  } else if (flags & JS_DEBUG_TRACE_DEBUGGER_STMT) {
    reason = "debuggerStatement";
  } else if (agent->mode == MODE_PAUSE_NEXT) {
    reason = "other";
  } else if (agent->mode == MODE_STEP_INTO) {
    reason = "step";
  } else if (agent->mode == MODE_STEP_OVER && depth <= agent->step_depth) {
    reason = "step";
  } else if (agent->mode == MODE_STEP_OUT && depth < agent->step_depth) {
    reason = "step";
  }

  Breakpoint *bp = NULL;
  if (filename != JS_ATOM_NULL && any_breakpoint_on_line(agent, line)) {
    const char *file = JS_AtomToCString(ctx, filename);
    bp = breakpoint_at(agent, file, line);
    JS_FreeCString(ctx, file);
  }
  /* jsui: one stop per breakpoint per visit of its line. The other
     statements on the line in the same call (and callbacks it runs) match it
     too and would stop again right after resuming. */
  if (bp && bp->id == agent->last_bp_id && depth >= agent->last_bp_depth) {
    bp = NULL;
  } else if (!bp && depth <= agent->last_bp_depth) {
    agent->last_bp_id = 0;
    agent->last_bp_depth = 0;
  }
  if (!reason && bp && bp->condition) {
    agent->evaluating = true;
    JSValue v = JS_EvalInStackFrame(ctx, 0, bp->condition,
                                    strlen(bp->condition), "<condition>");
    agent->evaluating = false;
    int truthy = JS_IsException(v) ? 0 : JS_ToBool(ctx, v);
    if (JS_IsException(v) || truthy < 0)
      JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    if (truthy <= 0) bp = NULL;
  }
  if (bp) {
    agent->last_bp_id = bp->id;
    agent->last_bp_depth = depth;
    if (!reason) {
      reason = "other";
      breakpoint_id = bp->id;
    }
  }

  if (reason) pause_until_resumed(agent, reason, breakpoint_id);
  return 0;
}

static void resume_with(QJSCDPAgent *agent, PauseMode mode) {
  agent->mode = mode;
  agent->step_depth = JS_GetStackDepth(agent->ctx);
  cdp_mutex_lock(&agent->lock);
  agent->resumed = true;
  cdp_cond_broadcast(&agent->cond);
  cdp_mutex_unlock(&agent->lock);
}

/* ------------------------------------------------------------- methods --- */

/* Both engines can already read and write JSON, so a value crosses between
   them as text rather than through a converter of our own. */
static JSValue protocol_value_to_debuggee(QJSCDPAgent *agent, JSValue v) {
  JSValue text = JS_JSONStringify(agent->jctx, v, JS_UNDEFINED, JS_UNDEFINED);
  size_t len = 0;
  const char *s = JS_ToCStringLen(agent->jctx, &len, text);
  JSValue out = JS_UNDEFINED;
  if (s) {
    out = JS_ParseJSON(agent->ctx, s, len, "<argument>");
    if (JS_IsException(out)) {
      JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
      out = JS_UNDEFINED;
    }
    JS_FreeCString(agent->jctx, s);
  }
  JS_FreeValue(agent->jctx, text);
  return out;
}

/* jsui: a debuggee value as a protocol value (returnByValue), through JSON
   text like protocol_value_to_debuggee. Values JSON cannot carry come back as
   undefined. */
static JSValue debuggee_to_protocol_value(QJSCDPAgent *agent, JSValue v) {
  JSValue text = JS_JSONStringify(agent->ctx, v, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(text)) {
    JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
    return JS_UNDEFINED;
  }
  size_t len = 0;
  const char *s = JS_IsString(text) ? JS_ToCStringLen(agent->ctx, &len, text) : NULL;
  JSValue out = JS_UNDEFINED;
  if (s) {
    out = JS_ParseJSON(agent->jctx, s, len, "<value>");
    if (JS_IsException(out)) {
      JS_FreeValue(agent->jctx, JS_GetException(agent->jctx));
      out = JS_UNDEFINED;
    }
    JS_FreeCString(agent->ctx, s);
  }
  JS_FreeValue(agent->ctx, text);
  return out;
}

static const char *type_name(QJSCDPAgent *agent, JSValue v) {
  if (JS_IsUndefined(v)) return "undefined";
  if (JS_IsBool(v)) return "boolean";
  if (JS_IsNumber(v)) return "number";
  if (JS_IsString(v)) return "string";
  if (JS_IsBigInt(v)) return "bigint";
  if (JS_IsSymbol(v)) return "symbol";
  if (JS_IsFunction(agent->ctx, v)) return "function";
  return "object";
}

/* Takes ownership of value. Answers with the CDP shape shared by
   Runtime.evaluate, Debugger.evaluateOnCallFrame and (jsui)
   Runtime.callFunctionOn; `by_value` is their returnByValue. */
static void send_evaluation_ex(QJSCDPAgent *agent, int id, JSValue value,
                               const char *group, bool by_value) {
  JSValue result = json_object(agent);
  if (!JS_IsException(value) && by_value) {
    JSValue r = json_object(agent);
    json_set_string(agent, r, "type", type_name(agent, value));
    JSValue pv = debuggee_to_protocol_value(agent, value);
    if (!JS_IsUndefined(pv)) json_set(agent, r, "value", pv);
    json_set(agent, result, "result", r);
    JS_FreeValue(agent->ctx, value);
    send_result(agent, id, result);
    return;
  }
  if (JS_IsException(value)) {
    JSValue exc = JS_GetException(agent->ctx);
    json_set(agent, result, "result", to_remote_object(agent, exc, group));
    json_set(
        agent, result, "exceptionDetails",
        to_exception_details(agent, JS_DupValue(agent->ctx, exc)));
    JS_FreeValue(agent->ctx, exc);
  } else {
    json_set(agent, result, "result", to_remote_object(agent, value, group));
  }
  JS_FreeValue(agent->ctx, value);
  send_result(agent, id, result);
}

/* jsui: one Runtime.CallArgument as a debuggee value. */
static JSValue call_argument(QJSCDPAgent *agent, JSValue arg) {
  const char *object_id = json_get_string(agent, arg, "objectId");
  if (object_id) {
    RemoteObject *r = find_object(agent, object_id);
    JS_FreeCString(agent->jctx, object_id);
    return r ? JS_DupValue(agent->ctx, r->value) : JS_UNDEFINED;
  }
  const char *special = json_get_string(agent, arg, "unserializableValue");
  if (special) {
    JSValue out = JS_UNDEFINED;
    if (strcmp(special, "NaN") == 0) out = JS_NewFloat64(agent->ctx, NAN);
    else if (strcmp(special, "Infinity") == 0) out = JS_NewFloat64(agent->ctx, INFINITY);
    else if (strcmp(special, "-Infinity") == 0) out = JS_NewFloat64(agent->ctx, -INFINITY);
    else if (strcmp(special, "-0") == 0) out = JS_NewFloat64(agent->ctx, -0.0);
    else {
      size_t n = strlen(special);
      if (n > 1 && special[n - 1] == 'n')
        out = JS_Eval(agent->ctx, special, n, "<argument>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(out)) {
        JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
        out = JS_UNDEFINED;
      }
    }
    JS_FreeCString(agent->jctx, special);
    return out;
  }
  JSValue v = json_get(agent, arg, "value");
  JSValue out = JS_IsUndefined(v) ? JS_UNDEFINED : protocol_value_to_debuggee(agent, v);
  JS_FreeValue(agent->jctx, v);
  return out;
}

/* jsui: Runtime.callFunctionOn. DevTools and VS Code call small functions on
   objects to preview, expand and format them. */
static void call_function_on(QJSCDPAgent *agent, int id, JSValue params) {
  const char *decl = json_get_string(agent, params, "functionDeclaration");
  if (!decl) {
    send_error(agent, id, "Runtime.callFunctionOn needs a functionDeclaration");
    return;
  }
  const char *object_id = json_get_string(agent, params, "objectId");
  const char *group = json_get_string(agent, params, "objectGroup");
  bool by_value = json_get_bool(agent, params, "returnByValue", false);
  JSValue self = JS_UNDEFINED;
  if (object_id) {
    RemoteObject *r = find_object(agent, object_id);
    if (r) self = JS_DupValue(agent->ctx, r->value);
  }

  size_t dlen = strlen(decl);
  char *src = malloc(dlen + 3);
  src[0] = '(';
  memcpy(src + 1, decl, dlen);
  src[dlen + 1] = ')';
  src[dlen + 2] = '\0';

  bool was = agent->evaluating;
  agent->evaluating = true;
  JSValue fn = JS_Eval(agent->ctx, src, dlen + 2, "<callFunctionOn>",
                       JS_EVAL_TYPE_GLOBAL);
  free(src);
  JSValue result;
  if (JS_IsException(fn)) {
    result = fn;
  } else {
    JSValue list = json_get(agent, params, "arguments");
    int argc = JS_IsObject(list) ? json_get_int(agent, list, "length", 0) : 0;
    if (argc > 64) argc = 64;
    JSValue argv[64];
    for (int i = 0; i < argc; i++) {
      JSValue a = JS_GetPropertyUint32(agent->jctx, list, (uint32_t)i);
      argv[i] = call_argument(agent, a);
      JS_FreeValue(agent->jctx, a);
    }
    JS_FreeValue(agent->jctx, list);
    result = JS_Call(agent->ctx, fn, self, argc, argv);
    for (int i = 0; i < argc; i++) JS_FreeValue(agent->ctx, argv[i]);
    JS_FreeValue(agent->ctx, fn);
  }
  agent->evaluating = was;
  JS_FreeValue(agent->ctx, self);
  send_evaluation_ex(agent, id, result, group, by_value);
  if (object_id) JS_FreeCString(agent->jctx, object_id);
  if (group) JS_FreeCString(agent->jctx, group);
  JS_FreeCString(agent->jctx, decl);
}

static void send_properties(QJSCDPAgent *agent, int id, JSValue params) {
  const char *object_id = json_get_string(agent, params, "objectId");
  RemoteObject *remote = find_object(agent, object_id);
  if (object_id) JS_FreeCString(agent->jctx, object_id);
  if (!remote) {
    send_error(agent, id, "No object found for the given objectId");
    return;
  }
  /* jsui: children live in their parent's group, so releasing the group
     (which the frontends do when they move on) releases them too. */
  const char *group = remote->group;
  JSValue target = JS_DupValue(agent->ctx, remote->value);
  bool accessors_only =
      json_get_bool(agent, params, "accessorPropertiesOnly", false);
  bool own_only = json_get_bool(agent, params, "ownProperties", false);

  /* Not JS_GPN_ENUM_ONLY: the frontend asked for own properties and expects the
     non-enumerable ones too, which is how an empty array still has a `length`
     to show. */
  JSPropertyEnum *props = NULL;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(
          agent->ctx, &props, &count, target, JS_GPN_STRING_MASK) < 0) {
    JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
    JS_FreeValue(agent->ctx, target);
    send_error(agent, id, "Could not read the object's properties");
    return;
  }

  JSValue list = json_array(agent);
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; i++) {
    const char *name = JS_AtomToCString(agent->ctx, props[i].atom);

    JSPropertyDescriptor desc;
    int described =
        JS_GetOwnProperty(agent->ctx, &desc, target, props[i].atom);
    if (described < 0) JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
    if (accessors_only && !(described > 0 && (desc.flags & JS_PROP_GETSET))) {
      if (described > 0) {
        JS_FreeValue(agent->ctx, desc.value);
        JS_FreeValue(agent->ctx, desc.getter);
        JS_FreeValue(agent->ctx, desc.setter);
      }
      if (name) JS_FreeCString(agent->ctx, name);
      continue;
    }

    JSValue entry = json_object(agent);
    json_set_string(agent, entry, "name", name ? name : "");
    json_set_bool(agent, entry, "isOwn", true);
    json_set_bool(
        agent, entry, "writable",
        described > 0 && (desc.flags & JS_PROP_WRITABLE) != 0);
    json_set_bool(
        agent, entry, "configurable",
        described > 0 && (desc.flags & JS_PROP_CONFIGURABLE) != 0);
    json_set_bool(
        agent, entry, "enumerable",
        described > 0 && (desc.flags & JS_PROP_ENUMERABLE) != 0);

    if (described > 0 && (desc.flags & JS_PROP_GETSET)) {
      /* An accessor is described, not invoked. Reading it to fill in a value
         would run the program's own code to answer a question about it. */
      if (!JS_IsUndefined(desc.getter))
        json_set(
            agent, entry, "get", to_remote_object(agent, desc.getter, group));
      if (!JS_IsUndefined(desc.setter))
        json_set(
            agent, entry, "set", to_remote_object(agent, desc.setter, group));
    } else if (described > 0) {
      json_set(
          agent, entry, "value", to_remote_object(agent, desc.value, group));
    }
    if (described > 0) {
      JS_FreeValue(agent->ctx, desc.value);
      JS_FreeValue(agent->ctx, desc.getter);
      JS_FreeValue(agent->ctx, desc.setter);
    }

    json_set_index(agent, list, n++, entry);
    if (name) JS_FreeCString(agent->ctx, name);
  }
  JS_FreePropertyEnum(agent->ctx, props, count);

  JSValue result = json_object(agent);
  json_set(agent, result, "result", list);
  /* jsui: the prototype, which is how the frontends show inherited members. */
  if (own_only && !accessors_only && JS_IsObject(target)) {
    JSValue proto = JS_GetPrototype(agent->ctx, target);
    if (JS_IsException(proto)) {
      JS_FreeValue(agent->ctx, JS_GetException(agent->ctx));
    } else if (JS_IsObject(proto)) {
      JSValue internals = json_array(agent);
      JSValue entry = json_object(agent);
      json_set_string(agent, entry, "name", "[[Prototype]]");
      json_set(agent, entry, "value", to_remote_object(agent, proto, group));
      json_set_index(agent, internals, 0, entry);
      json_set(agent, result, "internalProperties", internals);
    }
    JS_FreeValue(agent->ctx, proto);
  }
  JS_FreeValue(agent->ctx, target);
  send_result(agent, id, result);
}

/* Adds a breakpoint and answers with its id and the scripts it binds in.
   jsui: by url, by urlRegex, or (Debugger.setBreakpoint) at a script's exact
   url. */
static void add_breakpoint(QJSCDPAgent *agent, int id, const char *url,
                           const char *url_regex, const char *condition,
                           int line0, int col0, bool single_location) {
  agent->bps = realloc(agent->bps, sizeof(Breakpoint) * (agent->nbps + 1));
  Breakpoint *bp = &agent->bps[agent->nbps++];
  bp->id = ++agent->next_bp_id;
  bp->url = url ? strdup(url) : NULL;
  bp->url_regex = !url && url_regex ? strdup(url_regex) : NULL;
  bp->condition = condition && *condition ? strdup(condition) : NULL;
  bp->line = line0 + 1;
  bp->col = col0 + 1;
  bp->resolved = false;

  char bid[16];
  snprintf(bid, sizeof(bid), "%d", bp->id);
  JSValue locations = json_array(agent);
  uint32_t n = 0;
  for (int i = 0; i < agent->nscripts; i++) {
    if (!bp_matches(agent, bp, agent->scripts[i].url)) continue;
    bp->resolved = true;
    char sid[16];
    snprintf(sid, sizeof(sid), "%d", agent->scripts[i].id);
    JSValue loc = json_object(agent);
    json_set_string(agent, loc, "scriptId", sid);
    json_set_int(agent, loc, "lineNumber", bp->line - 1);
    json_set_int(agent, loc, "columnNumber", bp->col - 1);
    if (single_location) {
      JSValue result = json_object(agent);
      json_set_string(agent, result, "breakpointId", bid);
      json_set(agent, result, "actualLocation", loc);
      JS_FreeValue(agent->jctx, locations);
      send_result(agent, id, result);
      return;
    }
    json_set_index(agent, locations, n++, loc);
  }

  JSValue result = json_object(agent);
  json_set_string(agent, result, "breakpointId", bid);
  json_set(agent, result, "locations", locations);
  send_result(agent, id, result);
}

static void set_breakpoint_by_url(QJSCDPAgent *agent, int id, JSValue params) {
  const char *url = json_get_string(agent, params, "url");
  const char *url_regex = json_get_string(agent, params, "urlRegex");
  if (!url && !url_regex) {
    send_error(agent, id, "Debugger.setBreakpointByUrl needs a url or urlRegex");
    return;
  }
  const char *condition = json_get_string(agent, params, "condition");
  add_breakpoint(agent, id, url, url_regex, condition,
                 json_get_int(agent, params, "lineNumber", 0),
                 json_get_int(agent, params, "columnNumber", 0), false);
  if (url) JS_FreeCString(agent->jctx, url);
  if (url_regex) JS_FreeCString(agent->jctx, url_regex);
  if (condition) JS_FreeCString(agent->jctx, condition);
}

/* jsui: Debugger.setBreakpoint, at a location in one script. */
static void set_breakpoint(QJSCDPAgent *agent, int id, JSValue params) {
  JSValue loc = json_get(agent, params, "location");
  const char *script_id = json_get_string(agent, loc, "scriptId");
  Script *script = find_script_by_id(agent, script_id);
  if (!script) {
    send_error(agent, id, "No script with that scriptId");
  } else {
    const char *condition = json_get_string(agent, params, "condition");
    add_breakpoint(agent, id, script->url, NULL, condition,
                   json_get_int(agent, loc, "lineNumber", 0),
                   json_get_int(agent, loc, "columnNumber", 0), true);
    if (condition) JS_FreeCString(agent->jctx, condition);
  }
  if (script_id) JS_FreeCString(agent->jctx, script_id);
  JS_FreeValue(agent->jctx, loc);
}

/* jsui: Debugger.getPossibleBreakpoints. Breakpoints bind to lines, so every
   line from start to end (exclusive) is a candidate, at its first column. */
static void get_possible_breakpoints(QJSCDPAgent *agent, int id,
                                     JSValue params) {
  JSValue start = json_get(agent, params, "start");
  JSValue end = json_get(agent, params, "end");
  const char *script_id = json_get_string(agent, start, "scriptId");
  int from = json_get_int(agent, start, "lineNumber", 0);
  int col = json_get_int(agent, start, "columnNumber", 0);
  int to = JS_IsObject(end) ? json_get_int(agent, end, "lineNumber", from) : from;
  if (to <= from) to = from + 1;
  if (to - from > 1000) to = from + 1000;
  JSValue list = json_array(agent);
  for (int line = from; line < to; line++) {
    JSValue loc = json_object(agent);
    json_set_string(agent, loc, "scriptId", script_id ? script_id : "0");
    json_set_int(agent, loc, "lineNumber", line);
    json_set_int(agent, loc, "columnNumber", line == from ? col : 0);
    json_set_index(agent, list, (uint32_t)(line - from), loc);
  }
  JSValue result = json_object(agent);
  json_set(agent, result, "locations", list);
  send_result(agent, id, result);
  if (script_id) JS_FreeCString(agent->jctx, script_id);
  JS_FreeValue(agent->jctx, start);
  JS_FreeValue(agent->jctx, end);
}

static void remove_breakpoint(QJSCDPAgent *agent, JSValue params) {
  const char *bid = json_get_string(agent, params, "breakpointId");
  if (!bid) return;
  int wanted = atoi(bid);
  for (int i = 0; i < agent->nbps; i++) {
    if (agent->bps[i].id != wanted) continue;
    free(agent->bps[i].url);
    free(agent->bps[i].url_regex);
    free(agent->bps[i].condition);
    agent->bps[i] = agent->bps[--agent->nbps];
    break;
  }
  JS_FreeCString(agent->jctx, bid);
}

/* Writes a local in a frame that is stopped, so that the frontend's "edit
   value" reaches the live frame rather than a copy of it. */
static void set_variable_value(QJSCDPAgent *agent, int id, JSValue params) {
  const char *frame_id = json_get_string(agent, params, "callFrameId");
  const char *name = json_get_string(agent, params, "variableName");
  if (!frame_id || !name) {
    send_error(
        agent, id, "Debugger.setVariableValue needs a call frame and a name");
    goto done;
  }

  JSValue arg = json_get(agent, params, "newValue");
  JSValue wanted = json_get(agent, arg, "value");
  JSValue value = protocol_value_to_debuggee(agent, wanted);
  JS_FreeValue(agent->jctx, wanted);
  JS_FreeValue(agent->jctx, arg);

  int rc = JS_SetVariableAtLevel(agent->ctx, atoi(frame_id), name, value);
  if (rc == 0)
    send_empty_result(agent, id);
  else if (rc == -2)
    send_error(agent, id, "That binding is a constant");
  else
    send_error(agent, id, "No such variable in that call frame");

done:
  if (frame_id) JS_FreeCString(agent->jctx, frame_id);
  if (name) JS_FreeCString(agent->jctx, name);
}

/* ------------------------------------------------------------ dispatch --- */

void qjs_cdp_set_debugger_enabled(QJSCDPAgent *agent, bool enabled);

static bool is(const char *method, const char *name) {
  return strcmp(method, name) == 0;
}

/* Every method dispatch() answers. Kept beside it rather than derived from it
   because the frontend needs the answer before the message is queued, on
   another thread; CDPTest.EveryAdvertisedMethodIsImplemented keeps the two
   honest. */
static const char *const kMethods[] = {
    "Runtime.evaluate",
    "Runtime.getProperties",
    "Runtime.callFunctionOn",
    "Runtime.releaseObject",
    "Runtime.releaseObjectGroup",
    "Runtime.getHeapUsage",
    "Runtime.discardConsoleEntries",
    "Debugger.enable",
    "Debugger.disable",
    "Debugger.setBreakpointByUrl",
    "Debugger.setBreakpoint",
    "Debugger.getPossibleBreakpoints",
    "Debugger.removeBreakpoint",
    "Debugger.setBreakpointsActive",
    "Debugger.resume",
    "Debugger.stepOver",
    "Debugger.stepInto",
    "Debugger.stepOut",
    "Debugger.pause",
    "Debugger.evaluateOnCallFrame",
    "Debugger.setVariableValue",
    "Debugger.getScriptSource",
    "Debugger.setPauseOnExceptions",
    "Debugger.setAsyncCallStackDepth",
    "Debugger.setBlackboxPatterns",
    "Debugger.setBlackboxedRanges",
    "Debugger.setSkipAllPauses",
    "Profiler.enable", /* jsui */
    "Profiler.disable",
    "Profiler.setSamplingInterval",
    "Profiler.start",
    "Profiler.stop",
};

bool qjs_cdp_handles(const char *method) {
  if (!method) return false;
  for (size_t i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
    if (is(method, kMethods[i])) return true;
  return false;
}

static void dispatch(
    QJSCDPAgent *agent, int id, const char *method, JSValue params) {
  /* --- Runtime --- */
  if (is(method, "Runtime.evaluate")) {
    /* A contextId naming some other execution context is an invalid request,
       not an evaluation: answering it would run the expression in the wrong
       place and the frontend would never know. */
    const int wanted =
        json_get_int(agent, params, "contextId", agent->exec_ctx_id);
    if (wanted != agent->exec_ctx_id) {
      send_error_code(agent, id, -32600, "Unknown execution context id");
      return;
    }
    const char *expr = json_get_string(agent, params, "expression");
    const char *group = json_get_string(agent, params, "objectGroup");
    JSValue v = expr ? JS_Eval(
                           agent->ctx, expr, strlen(expr), "<evaluate>",
                           JS_EVAL_TYPE_GLOBAL)
                     : JS_UNDEFINED;
    send_evaluation_ex(agent, id, v, group,
                       json_get_bool(agent, params, "returnByValue", false));
    if (expr) JS_FreeCString(agent->jctx, expr);
    if (group) JS_FreeCString(agent->jctx, group);

  } else if (is(method, "Runtime.getProperties")) {
    send_properties(agent, id, params);

  } else if (is(method, "Runtime.callFunctionOn")) {
    call_function_on(agent, id, params);

  } else if (is(method, "Runtime.releaseObject")) {
    const char *object_id = json_get_string(agent, params, "objectId");
    const bool released = release_object(agent, object_id);
    if (object_id) JS_FreeCString(agent->jctx, object_id);
    /* Releasing something already released is an error, not a no-op: the
       frontend is telling us about a reference it believes it holds. */
    if (released)
      send_empty_result(agent, id);
    else
      send_error(agent, id, "No object found for the given objectId");

  } else if (is(method, "Runtime.releaseObjectGroup")) {
    const char *group = json_get_string(agent, params, "objectGroup");
    release_object_group(agent, group);
    if (group) JS_FreeCString(agent->jctx, group);
    send_empty_result(agent, id);

  } else if (is(method, "Runtime.getHeapUsage")) {
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(JS_GetRuntime(agent->ctx), &usage);
    JSValue result = json_object(agent);
    json_set_int(agent, result, "usedSize", usage.memory_used_size);
    json_set_int(agent, result, "totalSize", usage.malloc_size);
    send_result(agent, id, result);

  } else if (
      is(method, "Runtime.discardConsoleEntries") ||
      is(method, "Runtime.setAsyncCallStackDepth") ||
      is(method, "Runtime.setCustomObjectFormatterEnabled") ||
      is(method, "Runtime.setMaxCallStackSizeToCapture")) {
    send_empty_result(agent, id);

    /* --- Debugger --- */
  } else if (is(method, "Debugger.enable")) {
    /* Enabling is safe to act on directly -- one more frontend watching cannot
       hurt another. DISABLING is not, which is why it is the embedder that
       turns this back off, once it knows no session wants it any more. */
    qjs_cdp_set_debugger_enabled(agent, true);
    for (int i = 0; i < agent->nscripts; i++)
      announce_script(agent, &agent->scripts[i], false);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.disable")) {
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.setBreakpointByUrl")) {
    set_breakpoint_by_url(agent, id, params);

  } else if (is(method, "Debugger.setBreakpoint")) {
    set_breakpoint(agent, id, params);

  } else if (is(method, "Debugger.getPossibleBreakpoints")) {
    get_possible_breakpoints(agent, id, params);

  } else if (is(method, "Debugger.setSkipAllPauses")) {
    agent->skip_all_pauses = json_get_bool(agent, params, "skip", false);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.removeBreakpoint")) {
    remove_breakpoint(agent, params);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.setBreakpointsActive")) {
    agent->breakpoints_active = json_get_bool(agent, params, "active", true);
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.resume")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_RUN);

  } else if (is(method, "Debugger.stepOver")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_STEP_OVER);

  } else if (is(method, "Debugger.stepInto")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_STEP_INTO);

  } else if (is(method, "Debugger.stepOut")) {
    send_empty_result(agent, id);
    resume_with(agent, MODE_STEP_OUT);

  } else if (is(method, "Debugger.pause")) {
    agent->pause_pending = true;
    send_empty_result(agent, id);

  } else if (is(method, "Debugger.evaluateOnCallFrame")) {
    const char *frame_id = json_get_string(agent, params, "callFrameId");
    const char *expr = json_get_string(agent, params, "expression");
    const char *group = json_get_string(agent, params, "objectGroup");
    JSValue v = (frame_id && expr) ? JS_EvalInStackFrame(
                                         agent->ctx, atoi(frame_id), expr,
                                         strlen(expr), "<evaluate>")
                                   : JS_UNDEFINED;
    send_evaluation_ex(agent, id, v, group,
                       json_get_bool(agent, params, "returnByValue", false));
    if (frame_id) JS_FreeCString(agent->jctx, frame_id);
    if (expr) JS_FreeCString(agent->jctx, expr);
    if (group) JS_FreeCString(agent->jctx, group);

  } else if (is(method, "Debugger.setVariableValue")) {
    set_variable_value(agent, id, params);

  } else if (is(method, "Debugger.getScriptSource")) {
    const char *script_id = json_get_string(agent, params, "scriptId");
    Script *script = find_script_by_id(agent, script_id);
    if (script_id) JS_FreeCString(agent->jctx, script_id);
    if (!script) {
      send_error(agent, id, "No script with that scriptId");
    } else {
      JSValue result = json_object(agent);
      json_set_string(agent, result, "scriptSource", script->source);
      send_result(agent, id, result);
    }

  } else if (
      is(method, "Debugger.setPauseOnExceptions") ||
      is(method, "Debugger.setAsyncCallStackDepth") ||
      is(method, "Debugger.setBlackboxPatterns") ||
      is(method, "Debugger.setBlackboxedRanges")) {
    send_empty_result(agent, id);

    /* --- Profiler (jsui) --- */
  } else if (is(method, "Profiler.enable")) {
    send_empty_result(agent, id);
  } else if (is(method, "Profiler.disable")) {
    if (agent->profiling) JS_FreeValue(agent->jctx, profiler_stop(agent));
    send_empty_result(agent, id);
  } else if (is(method, "Profiler.setSamplingInterval")) {
    int us = json_get_int(agent, params, "interval", 1000);
    agent->sampling_interval_us = us < 100 ? 100 : us;
    send_empty_result(agent, id);
  } else if (is(method, "Profiler.start")) {
    if (!agent->profiling) profiler_start(agent);
    send_empty_result(agent, id);
  } else if (is(method, "Profiler.stop")) {
    if (!agent->profiling) {
      send_error(agent, id, "Profiling is not started");
    } else {
      JSValue result = json_object(agent);
      json_set(agent, result, "profile", profiler_stop(agent));
      send_result(agent, id, result);
    }
  }
}

static void handle_message(QJSCDPAgent *agent, const char *json, size_t len) {
  JSValue msg = JS_ParseJSON(agent->jctx, json, len, "<cdp>");
  if (JS_IsException(msg)) {
    JS_FreeValue(agent->jctx, JS_GetException(agent->jctx));
    JS_FreeValue(agent->jctx, msg);
    return;
  }

  int id = json_get_int(agent, msg, "id", 0);
  const char *method = json_get_string(agent, msg, "method");
  JSValue params = json_get(agent, msg, "params");
  if (!JS_IsObject(params)) {
    JS_FreeValue(agent->jctx, params);
    params = json_object(agent);
  }

  if (method)
    dispatch(agent, id, method, params);
  else
    send_error(agent, id, "Message had no method");
  update_arming(agent); /* jsui: breakpoints, steps or pauses changed */

  if (method) JS_FreeCString(agent->jctx, method);
  JS_FreeValue(agent->jctx, params);
  JS_FreeValue(agent->jctx, msg);
}

/* -------------------------------------------------------- the C API ------ */

QJSCDPAgent *qjs_cdp_new(
    JSContext *ctx, int execution_context_id, QJSCDPSendFunc *send,
    void *send_opaque) {
  QJSCDPAgent *agent = calloc(1, sizeof(*agent));
  agent->ctx = ctx;
  agent->exec_ctx_id = execution_context_id;
  agent->send = send;
  agent->send_opaque = send_opaque;
  agent->breakpoints_active = true;
  agent->mode = MODE_RUN;
  agent->sampling_interval_us = 1000; /* jsui: Profiler */
  cdp_mutex_init(&agent->lock);
  cdp_cond_init(&agent->cond);

  /* Base objects and JSON, nothing else: this runtime never runs a program. */
  agent->jrt = JS_NewRuntime();
  agent->jctx = JS_NewContextRaw(agent->jrt);
  JS_AddIntrinsicBaseObjects(agent->jctx);
  JS_AddIntrinsicJSON(agent->jctx);
  JS_AddIntrinsicRegExpCompiler(agent->jctx); /* jsui: urlRegex */
  JS_AddIntrinsicRegExp(agent->jctx);

  JS_SetDebugTraceHandler(ctx, on_statement, agent);
  JS_SetDebugTraceArmed(ctx, false);
  return agent;
}

void qjs_cdp_free(QJSCDPAgent *agent) {
  if (!agent) return;
  JS_SetDebugTraceHandler(agent->ctx, NULL, NULL);

  for (int i = 0; i < agent->nobjs; i++) {
    JS_FreeValue(agent->ctx, agent->objs[i].value);
    free(agent->objs[i].group);
  }
  for (int i = 0; i < agent->nscripts; i++) {
    free(agent->scripts[i].url);
    free(agent->scripts[i].source);
    free(agent->scripts[i].source_map_url);
  }
  for (int i = 0; i < agent->nbps; i++) {
    free(agent->bps[i].url);
    free(agent->bps[i].url_regex);
    free(agent->bps[i].condition);
  }
  for (int i = 0; i < agent->queue_len; i++) free(agent->queue[i]);
  profiler_reset(agent); /* jsui: its atoms are the debuggee's */
  free(agent->pnodes);
  free(agent->samples);
  free(agent->sample_times);
  free(agent->spans);
  free(agent->frames);

  free(agent->objs);
  free(agent->scripts);
  free(agent->bps);
  free(agent->queue);
  free(agent->queue_session);

  JS_FreeContext(agent->jctx);
  JS_FreeRuntime(agent->jrt);
  cdp_cond_destroy(&agent->cond);
  cdp_mutex_destroy(&agent->lock);
  free(agent);
}

void qjs_cdp_send_message(
    QJSCDPAgent *agent, void *session, const char *json, size_t len) {
  char *copy = malloc(len + 1);
  memcpy(copy, json, len);
  copy[len] = '\0';

  cdp_mutex_lock(&agent->lock);
  if (agent->queue_len == agent->queue_cap) {
    agent->queue_cap = agent->queue_cap ? agent->queue_cap * 2 : 8;
    agent->queue =
        realloc(agent->queue, sizeof(char *) * (size_t)agent->queue_cap);
    agent->queue_session = realloc(
        agent->queue_session, sizeof(void *) * (size_t)agent->queue_cap);
  }
  agent->queue_session[agent->queue_len] = session;
  agent->queue[agent->queue_len++] = copy;
  cdp_cond_broadcast(&agent->cond);
  cdp_mutex_unlock(&agent->lock);
}

void qjs_cdp_poll(QJSCDPAgent *agent) {
  enter_agent_runtime(agent);
  for (;;) {
    void *session = NULL;
    cdp_mutex_lock(&agent->lock);
    char *msg = take_message(agent, &session);
    cdp_mutex_unlock(&agent->lock);
    if (!msg) return;
    agent->session = session;
    handle_message(agent, msg, strlen(msg));
    agent->session = NULL;
    free(msg);
  }
}

void qjs_cdp_console_message(
    QJSCDPAgent *agent, const char *type, const JSValue *args, int count) {
  enter_agent_runtime(agent);
  JSValue list = json_array(agent);
  for (int i = 0; i < count; i++)
    json_set_index(
        agent, list, (uint32_t)i, to_remote_object(agent, args[i], NULL));

  JSValue params = json_object(agent);
  json_set_string(agent, params, "type", type ? type : "log");
  json_set(agent, params, "args", list);
  json_set_int(agent, params, "executionContextId", agent->exec_ctx_id);
  json_set(agent, params, "timestamp", JS_NewFloat64(agent->jctx, 0));
  send_event(agent, "Runtime.consoleAPICalled", params);
}

/* jsui: the value of a `//# <name>=<value>` comment among the last lines of
   a script (bundlers put sourceMappingURL last), or NULL. Caller frees. */
static char *trailing_comment(const char *source, const char *name) {
  size_t n = strlen(source), nlen = strlen(name);
  /* Only lines that end within the last 4 KiB (a data: URL map can be one
     long line). */
  size_t stop = n > 4096 ? n - 4096 : 0;
  size_t end = n;
  while (end > stop) {
    size_t start = end;
    while (start > 0 && source[start - 1] != '\n') start--;
    const char *line = source + start;
    size_t len = end - start;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if (len > 5 + nlen && line[0] == '/' && line[1] == '/' &&
        (line[2] == '#' || line[2] == '@') && line[3] == ' ' &&
        strncmp(line + 4, name, nlen) == 0 && line[4 + nlen] == '=') {
      size_t vlen = len - 5 - nlen;
      char *out = malloc(vlen + 1);
      memcpy(out, line + 5 + nlen, vlen);
      out[vlen] = '\0';
      return out;
    }
    if (start == 0) break;
    end = start - 1;
  }
  return NULL;
}

void qjs_cdp_script_loaded(
    QJSCDPAgent *agent, const char *url, const char *source) {
  enter_agent_runtime(agent);
  agent->scripts =
      realloc(agent->scripts, sizeof(Script) * (size_t)(agent->nscripts + 1));
  Script *script = &agent->scripts[agent->nscripts++];
  script->id = agent->nscripts;
  script->url = strdup(url ? url : "");
  script->source = strdup(source ? source : "");
  script->source_map_url = trailing_comment(script->source, "sourceMappingURL");
  char *source_url = trailing_comment(script->source, "sourceURL");
  script->has_source_url = source_url != NULL;
  free(source_url);

  if (agent->debugger_enabled) {
    announce_script(agent, script, true);
    resolve_breakpoints(agent, script);
  }
}

char *qjs_cdp_export_state(QJSCDPAgent *agent) {
  enter_agent_runtime(agent);
  JSValue list = json_array(agent);
  for (int i = 0; i < agent->nbps; i++) {
    Breakpoint *bp = &agent->bps[i];
    JSValue e = json_object(agent);
    json_set_int(agent, e, "id", bp->id);
    if (bp->url) json_set_string(agent, e, "url", bp->url);
    if (bp->url_regex) json_set_string(agent, e, "urlRegex", bp->url_regex);
    json_set_int(agent, e, "line", bp->line);
    json_set_int(agent, e, "col", bp->col);
    if (bp->condition) json_set_string(agent, e, "condition", bp->condition);
    json_set_index(agent, list, (uint32_t)i, e);
  }

  JSValue state = json_object(agent);
  json_set_bool(agent, state, "enabled", agent->debugger_enabled);
  json_set_int(agent, state, "nextBreakpointId", agent->next_bp_id);
  json_set(agent, state, "breakpoints", list);

  JSValue text =
      JS_JSONStringify(agent->jctx, state, JS_UNDEFINED, JS_UNDEFINED);
  size_t len = 0;
  const char *json = JS_ToCStringLen(agent->jctx, &len, text);
  char *out = NULL;
  if (json) {
    out = malloc(len + 1);
    memcpy(out, json, len);
    out[len] = '\0';
    JS_FreeCString(agent->jctx, json);
  }
  JS_FreeValue(agent->jctx, text);
  JS_FreeValue(agent->jctx, state);
  return out;
}

void qjs_cdp_import_state(QJSCDPAgent *agent, const char *json) {
  enter_agent_runtime(agent);
  if (!json) return;
  JSValue state = JS_ParseJSON(agent->jctx, json, strlen(json), "<state>");
  if (JS_IsException(state)) {
    JS_FreeValue(agent->jctx, JS_GetException(agent->jctx));
    JS_FreeValue(agent->jctx, state);
    return;
  }

  agent->debugger_enabled = json_get_bool(agent, state, "enabled", false);
  agent->next_bp_id = json_get_int(agent, state, "nextBreakpointId", 0);

  JSValue list = json_get(agent, state, "breakpoints");
  const int count = json_get_int(agent, list, "length", 0);
  for (int i = 0; i < count; i++) {
    JSValue e = JS_GetPropertyUint32(agent->jctx, list, (uint32_t)i);
    const char *url = json_get_string(agent, e, "url");
    const char *url_regex = json_get_string(agent, e, "urlRegex");
    if (url || url_regex) {
      const char *condition = json_get_string(agent, e, "condition");
      agent->bps = realloc(agent->bps, sizeof(Breakpoint) * (agent->nbps + 1));
      Breakpoint *bp = &agent->bps[agent->nbps++];
      bp->id = json_get_int(agent, e, "id", 0);
      bp->url = url ? strdup(url) : NULL;
      bp->url_regex = !url && url_regex ? strdup(url_regex) : NULL;
      bp->condition = condition ? strdup(condition) : NULL;
      bp->line = json_get_int(agent, e, "line", 1);
      bp->col = json_get_int(agent, e, "col", 1);
      /* Unresolved again on purpose: the scripts are gone with the old runtime,
         and each one that comes back re-announces the breakpoints it binds. */
      bp->resolved = false;
      if (condition) JS_FreeCString(agent->jctx, condition);
    }
    if (url) JS_FreeCString(agent->jctx, url);
    if (url_regex) JS_FreeCString(agent->jctx, url_regex);
    JS_FreeValue(agent->jctx, e);
  }
  JS_FreeValue(agent->jctx, list);
  JS_FreeValue(agent->jctx, state);
  update_arming(agent);
}

bool qjs_cdp_debugger_enabled(QJSCDPAgent *agent) {
  return agent->debugger_enabled;
}

void qjs_cdp_set_debugger_enabled(QJSCDPAgent *agent, bool enabled) {
  agent->debugger_enabled = enabled;
  /* Disarmed rather than uninstalled: uninstalling the handler would
     permanently un-instrument everything parsed afterwards. */
  update_arming(agent);
}

char *qjs_cdp_capture_stack_trace(QJSCDPAgent *agent, int frames_to_skip) {
  enter_agent_runtime(agent);
  JSValue frames = json_array(agent);
  const int depth = JS_GetStackDepth(agent->ctx);
  uint32_t n = 0;

  for (int level = frames_to_skip; level < depth; level++) {
    JSDebugFrameInfo info;
    if (JS_GetFrameInfoAtLevel(agent->ctx, level, &info) < 0) break;
    if (info.is_native) continue;

    const char *file = info.filename != JS_ATOM_NULL
                           ? JS_AtomToCString(agent->ctx, info.filename)
                           : NULL;
    const char *func = info.func_name != JS_ATOM_NULL
                           ? JS_AtomToCString(agent->ctx, info.func_name)
                           : NULL;
    Script *script = file ? find_script_by_url(agent, file) : NULL;
    char script_id[16];
    snprintf(script_id, sizeof(script_id), "%d", script ? script->id : 0);

    JSValue frame = json_object(agent);
    json_set_string(agent, frame, "functionName", frame_name(func));
    json_set_string(agent, frame, "scriptId", script_id);
    json_set_string(agent, frame, "url", file ? file : "");
    json_set_int(agent, frame, "lineNumber", info.line > 0 ? info.line - 1 : 0);
    json_set_int(agent, frame, "columnNumber", info.col > 0 ? info.col - 1 : 0);
    json_set_index(agent, frames, n++, frame);

    if (file) JS_FreeCString(agent->ctx, file);
    if (func) JS_FreeCString(agent->ctx, func);
  }

  JSValue trace = json_object(agent);
  json_set(agent, trace, "callFrames", frames);

  JSValue text =
      JS_JSONStringify(agent->jctx, trace, JS_UNDEFINED, JS_UNDEFINED);
  size_t len = 0;
  const char *json = JS_ToCStringLen(agent->jctx, &len, text);
  char *out = NULL;
  if (json) {
    out = malloc(len + 1);
    memcpy(out, json, len);
    out[len] = '\0';
    JS_FreeCString(agent->jctx, json);
  }
  JS_FreeValue(agent->jctx, text);
  JS_FreeValue(agent->jctx, trace);
  return out;
}

void qjs_cdp_exception_thrown(QJSCDPAgent *agent, JSValue exception,
                              const char *text) {
  enter_agent_runtime(agent);
  JSValue details =
      to_exception_details(agent, JS_DupValue(agent->ctx, exception));
  if (text) json_set_string(agent, details, "text", text);
  JSValue params = json_object(agent);
  json_set(agent, params, "timestamp", JS_NewFloat64(agent->jctx, 0));
  json_set(agent, params, "exceptionDetails", details);
  send_event(agent, "Runtime.exceptionThrown", params);
}

void qjs_cdp_set_execution_context(QJSCDPAgent *agent, int id) {
  agent->exec_ctx_id = id;
}

void qjs_cdp_pause(QJSCDPAgent *agent) {
  /* jsui: a flag write only; the agent's runtime belongs to the JS thread. */
  agent->pause_pending = true;
  if (agent->debugger_enabled) JS_SetDebugTraceArmed(agent->ctx, true);
}

/* jsui: Profiler */
bool qjs_cdp_profiling(QJSCDPAgent *agent) { return agent->profiling; }

int qjs_cdp_sampling_interval(QJSCDPAgent *agent) {
  return agent->sampling_interval_us;
}

void qjs_cdp_profiler_tick(QJSCDPAgent *agent) {
  /* A flag write only, as qjs_cdp_pause; the next statement samples. */
  if (!agent->profiling) return;
  agent->sample_pending = true;
  JS_SetDebugTraceArmed(agent->ctx, true);
}

void qjs_cdp_profiler_idle(QJSCDPAgent *agent) {
  if (!agent->profiling) return;
  if (agent->nsamples && agent->samples[agent->nsamples - 1] == PROF_IDLE)
    return;
  profiler_push(agent, PROF_IDLE);
  /* The first statement JS runs next ends the idle stretch. */
  agent->sample_pending = true;
  JS_SetDebugTraceArmed(agent->ctx, true);
}

void qjs_cdp_profiler_set_clock(QJSCDPAgent *agent, QJSCDPClockFunc *now_us) {
  agent->clock = now_us;
}

void qjs_cdp_profiler_add_span(
    QJSCDPAgent *agent, const char *name, int64_t start_us, int64_t end_us) {
  if (!agent->profiling || end_us <= start_us || end_us <= agent->profile_start)
    return;
  int node = -1;
  for (int c = agent->pnodes[PROF_ROOT].first_child; c >= 0;
       c = agent->pnodes[c].next_sibling)
    if (agent->pnodes[c].label && strcmp(agent->pnodes[c].label, name) == 0)
      node = c;
  if (node < 0) {
    /* A key no function has, so profiler_node makes a new node. */
    int key = -3 - agent->npnodes;
    node = profiler_node(agent, PROF_ROOT, JS_ATOM_NULL, JS_ATOM_NULL, key, key);
    agent->pnodes[node].label = strdup(name);
  }
  if (agent->nspans == agent->spans_cap) {
    agent->spans_cap = agent->spans_cap ? agent->spans_cap * 2 : 1024;
    agent->spans =
        realloc(agent->spans, sizeof(ProfSpan) * (size_t)agent->spans_cap);
  }
  agent->spans[agent->nspans].node = node;
  agent->spans[agent->nspans].start = start_us;
  agent->spans[agent->nspans].end = end_us;
  agent->nspans++;
}
