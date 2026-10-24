# The quickjs.c block of cmake/Deps.cmake on main (pre-#93), as a script: cmake -DIN=quickjs.c -DOUT=out.c -P jsui-patch.cmake
set(_qjs_c "${IN}")
file(READ ${_qjs_c} _qjs)
set(_qjs_anchor "#define JS_GC_MARK(p)   (js_rc(p)->mark)\n")
set(_qjs_defs
  "void JS_FreeValueRT(JSRuntime *rt, JSValue v)\n{"
  "void JS_FreeValue(JSContext *ctx, JSValue v)\n{"
  "JSValue JS_DupValue(JSContext *ctx, JSValueConst v)\n{"
  "JSValue JS_DupValueRT(JSRuntime *rt, JSValueConst v)\n{")
set(_qjs_ok TRUE)
string(FIND "${_qjs}" "${_qjs_anchor}" _qjs_at)
if(_qjs_at LESS 0)
  set(_qjs_ok FALSE)
endif()
foreach(_d IN LISTS _qjs_defs)
  string(FIND "${_qjs}" "${_d}" _qjs_at)
  if(_qjs_at LESS 0)
    set(_qjs_ok FALSE)
  endif()
endforeach()
if(_qjs_ok)
  foreach(_d IN LISTS _qjs_defs)
    # "void JS_FreeValue(" -> "void (JS_FreeValue)(": not expanded by the macros below.
    string(REGEX REPLACE "^([A-Za-z]+) (JS_[A-Za-z]+)\\(" "\\1 (\\2)(" _r "${_d}")
    string(REPLACE "${_d}" "${_r}" _qjs "${_qjs}")
  endforeach()
  string(REPLACE "${_qjs_anchor}" "${_qjs_anchor}
/* jsui: inline refcount fast paths (see cmake/Deps.cmake). */
#if defined(_MSC_VER) && !defined(__clang__)
#define jsui_inline static __forceinline
#else
#define jsui_inline static inline __attribute__((always_inline))
#endif
static void js_free_value_rt(JSRuntime *rt, JSValue v);
jsui_inline void jsui_free_value_rt(JSRuntime *rt, JSValue v) {
    if (JS_VALUE_HAS_REF_COUNT(v)) {
        void *p = JS_VALUE_GET_PTR(v);
        if (--JS_REF_COUNT(p) <= 0)
            js_free_value_rt(rt, v);
    }
}
jsui_inline JSValue jsui_dup_value(JSValueConst v) {
    if (JS_VALUE_HAS_REF_COUNT(v))
        JS_REF_COUNT(JS_VALUE_GET_PTR(v))++;
    return v;
}
#define JS_FreeValueRT(rt, v) jsui_free_value_rt(rt, v)
#define JS_FreeValue(ctx, v) jsui_free_value_rt((ctx)->rt, v)
#define JS_DupValueRT(rt, v) jsui_dup_value(v)
#define JS_DupValue(ctx, v) jsui_dup_value(v)
" _qjs "${_qjs}")
  # === / !== on objects, symbols, booleans, null and undefined (React's
  # bread and butter) are decided inline instead of in js_strict_eq_slow.
  set(_qjs_eq_anchor "static __exception int js_operator_in(JSContext *ctx, JSValue *sp)\n")
  set(_qjs_eq_ops
    "            OP_CMP(OP_strict_eq, ==, js_strict_eq_slow(ctx, sp, 0));\n            OP_CMP(OP_strict_neq, !=, js_strict_eq_slow(ctx, sp, 1));\n")
  string(FIND "${_qjs}" "${_qjs_eq_anchor}" _qjs_at1)
  string(FIND "${_qjs}" "${_qjs_eq_ops}" _qjs_at2)
  if(_qjs_at1 GREATER -1 AND _qjs_at2 GREATER -1)
    string(REPLACE "${_qjs_eq_anchor}" "/* jsui: strict equality without a call for the common tags. */
jsui_inline int jsui_strict_eq(JSContext *ctx, JSValue *sp, bool is_neq)
{
    JSValue a = sp[-2], b = sp[-1];
    int ta = JS_VALUE_GET_NORM_TAG(a), tb = JS_VALUE_GET_NORM_TAG(b);
    bool res;
    if (ta == tb && (ta == JS_TAG_OBJECT || ta == JS_TAG_SYMBOL))
        res = JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
    else if (ta == JS_TAG_NULL || ta == JS_TAG_UNDEFINED || tb == JS_TAG_NULL || tb == JS_TAG_UNDEFINED)
        res = ta == tb;
    else if (ta == JS_TAG_BOOL && tb == JS_TAG_BOOL)
        res = JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b);
    else
        return js_strict_eq_slow(ctx, sp, is_neq);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    sp[-2] = js_bool(res ^ is_neq);
    return 0;
}

${_qjs_eq_anchor}" _qjs "${_qjs}")
    string(REPLACE "${_qjs_eq_ops}"
      "            OP_CMP(OP_strict_eq, ==, jsui_strict_eq(ctx, sp, 0));\n            OP_CMP(OP_strict_neq, !=, jsui_strict_eq(ctx, sp, 1));\n"
      _qjs "${_qjs}")
  else()
    message(WARNING "quickjs.c changed: building it without the strict equality fast path")
  endif()
  message(STATUS "eq-fastpath: at1=${_qjs_at1} at2=${_qjs_at2}")
  file(CONFIGURE OUTPUT "${OUT}" CONTENT "${_qjs}" @ONLY)
  message(STATUS "patched OK")
else()
  message(WARNING "inline refcount anchor missing")
endif()
