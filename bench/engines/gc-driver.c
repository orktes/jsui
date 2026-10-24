// Host-driven gc-bench: runs out/gc-bench.js frame by frame from C, so collections can run between
// frames with an empty JS stack (JS_RunIdleGC refuses to run under JS). Built against a JSUI build
// tree's patched QuickJS (see RESULTS-rnquickjs.md):
//   gc-driver gc.js default|gen|gen-deferred|gen-deferred-2ms|jsui-policy
// jsui-policy: the WIP runtime's settings and its collectIdle policy between frames.
// gen-deferred: JS_SetGCGenerational + JS_SetGCDeferred(rt, true, 0), JS_RunIdleGC once between frames.
// gen-deferred-2ms: also JS_SetGCYoungBudget(rt, 2.0), young collections in ~2 ms slices.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"

#define WARM 20
#define FRAMES 300

static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  for (int i = 0; i < argc; i++) { const char *s = JS_ToCString(ctx, argv[i]); printf("%s%s", i ? " " : "", s); JS_FreeCString(ctx, s); }
  printf("\n"); return JS_UNDEFINED;
}
static void stats(const char *what, double *a, int n) {
  double s = 0; for (int i = 0; i < n; i++) s += a[i];
  qsort(a, n, sizeof *a, cmp);
  printf("%s p50=%.2f p95=%.2f p99=%.2f max=%.2f mean=%.2f total=%.1f\n", what, a[n / 2], a[n * 95 / 100], a[n * 99 / 100], a[n - 1], s / n, s);
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  const char *mode = argv[2];
  FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
  char *src = malloc(len + 1); fread(src, 1, len, f); src[len] = 0; fclose(f);
  JSRuntime *rt = JS_NewRuntime();
  int idle = 0;
  if (!strncmp(mode, "gen", 3)) JS_SetGCGenerational(rt, true);
  if (!strncmp(mode, "gen-deferred", 12)) { JS_SetGCDeferred(rt, true, 0); idle = 1; }
#ifdef HAVE_YOUNG_BUDGET
  if (!strcmp(mode, "gen-deferred-2ms")) JS_SetGCYoungBudget(rt, 2.0); /* JSUI's kGcSliceMs */
#endif
#ifdef HAVE_JSUI_POLICY
  int policy = !strcmp(mode, "jsui-policy");
  if (policy) { JS_SetGCGenerational(rt, true); JS_SetGCDeferred(rt, true, 0); JS_SetGCYoungBudget(rt, 2.0); idle = 2; }
  int waitTicks = 0, fullWaitTicks = 0;
#endif
  JSContext *ctx = JS_NewContext(rt);
  JSValue g = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
  JS_SetPropertyStr(ctx, g, "__gcDriven", JS_TRUE);
  JSValue r = JS_Eval(ctx, src, len, argv[1], JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); const char *s = JS_ToCString(ctx, e); fprintf(stderr, "%s\n", s); return 1; }
  JS_FreeValue(ctx, r);
  JSValue fn = JS_GetPropertyStr(ctx, g, "__gcFrame");
  static double frame[FRAMES], gc[FRAMES];
  JSGCStats st; JS_GetGCStats(rt, &st, true);
  for (int i = 0; i < WARM + FRAMES; i++) {
    if (i == WARM) JS_GetGCStats(rt, &st, true);
    JSValue arg = JS_NewInt32(ctx, i);
    double t0 = ms();
    JSValue v = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    double t1 = ms();
    if (JS_IsException(v)) { fprintf(stderr, "frame threw\n"); return 1; }
    JS_FreeValue(ctx, v);
    double t2 = t1;
    if (idle == 1) { JS_RunIdleGC(rt); t2 = ms(); }
#ifdef HAVE_JSUI_POLICY
    /* QuickJsEngine::collectIdle at 45b7f07: light = tick under 4 ms; idle never (every frame mounts) */
    if (idle == 2) {
      int light = t1 - t0 < 4.0;
      if (JS_HasPendingGC(rt)) {
        if (light) { JS_RunPendingGC(rt); waitTicks = 0; }
        else if (++waitTicks >= 3) { JS_FinishPendingGC(rt); waitTicks = 0; }
      } else {
        waitTicks = 0;
        if (!JS_IsFullGCDue(rt)) fullWaitTicks = 0;
        else if (++fullWaitTicks, (light && fullWaitTicks >= 10) || fullWaitTicks >= 30) { fullWaitTicks = 0; JS_RunGC(rt); }
      }
      t2 = ms();
    }
#endif
    if (i >= WARM) { frame[i - WARM] = t1 - t0; gc[i - WARM] = t2 - t1; }
  }
  JS_GetGCStats(rt, &st, false);
  printf("mode %s\n", mode);
  stats("[gc] frame ms:", frame, FRAMES);
  if (idle) stats("[gc] between-frame GC ms:", gc, FRAMES);
  printf("[gc] collections %llu (full %llu), gc total %.1f ms, max pause %.2f ms\n", (unsigned long long)st.count,
         (unsigned long long)st.full_count, st.total_ns / 1e6, st.max_ns / 1e6);
  JS_FreeValue(ctx, fn); JS_FreeValue(ctx, g);
  JS_FreeContext(ctx); JS_FreeRuntime(rt);
  return 0;
}
