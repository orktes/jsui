# Third-party dependencies.
#
#   QuickJS-ng  - JS engine (C, builds everywhere incl. Emscripten)
#   Yoga        - flexbox layout (C++)
#   Skia        - 2D rendering. Native builds use the prebuilt static
#                 libraries published by the Aseprite project (m124). The WASM
#                 build uses a Skia build made with scripts/build-skia-wasm.sh.
#
# Override any of these with -DJSUI_SKIA_DIR=/path/to/skia etc.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---------------------------------------------------------------- QuickJS-ng
FetchContent_Declare(quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG v0.17.0
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(quickjs)
if(NOT quickjs_POPULATED)
  FetchContent_Populate(quickjs)
endif()
# Engine patches go onto a copy of the sources in the build tree, which is
# what gets compiled; the fetched checkout stays pristine. Applied with
# `git apply`, in file name order:
#   third_party/quickjs-rnq   react-native-quickjs's patches (MIT; what each
#                             does and why: UPSTREAM-README.md there): inline
#                             caches, object literal shape templates, fused
#                             opcodes, dense arrays with holes, string caches,
#                             deferred GC, the debugger interface
#   third_party/quickjs-jsui  JSUI's, on top of those: generational cycle
#                             collection (JsEngine collects between frames)
set(_qjs_src ${CMAKE_BINARY_DIR}/quickjs-patched)
file(GLOB _qjs_patches ${CMAKE_CURRENT_LIST_DIR}/../third_party/quickjs-rnq/*.patch)
file(GLOB _qjs_jsui_patches ${CMAKE_CURRENT_LIST_DIR}/../third_party/quickjs-jsui/*.patch)
list(SORT _qjs_patches)
list(SORT _qjs_jsui_patches)
list(APPEND _qjs_patches ${_qjs_jsui_patches})
# Editing a patch re-runs the configure step (and so applies the patches again).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_qjs_patches})
set(_qjs_stamp_text "")
foreach(_p IN LISTS _qjs_patches)
  file(SHA256 ${_p} _h)
  string(APPEND _qjs_stamp_text "${_h}\n")
endforeach()
file(GLOB _qjs_top LIST_DIRECTORIES false ${quickjs_SOURCE_DIR}/*.c ${quickjs_SOURCE_DIR}/*.h)
foreach(_f IN LISTS _qjs_top)
  file(SHA256 ${_f} _h)
  string(APPEND _qjs_stamp_text "${_h}\n")
endforeach()
set(_qjs_stamp ${_qjs_src}/.jsui-patches)
set(_qjs_old_stamp "")
if(EXISTS ${_qjs_stamp})
  file(READ ${_qjs_stamp} _qjs_old_stamp)
endif()
if(NOT _qjs_old_stamp STREQUAL _qjs_stamp_text)
  find_package(Git REQUIRED)
  file(REMOVE_RECURSE ${_qjs_src})
  # The whole tree: a few patches also touch its tests and generated files.
  file(COPY ${quickjs_SOURCE_DIR}/ DESTINATION ${_qjs_src} PATTERN .git EXCLUDE)
  # Its own (empty) repository, so `git apply` resolves the patch paths here
  # even when the build tree is inside another checkout.
  execute_process(COMMAND ${GIT_EXECUTABLE} init -q WORKING_DIRECTORY ${_qjs_src} RESULT_VARIABLE _r)
  foreach(_p IN LISTS _qjs_patches)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${_p}
      WORKING_DIRECTORY ${_qjs_src} RESULT_VARIABLE _r ERROR_VARIABLE _e)
    if(NOT _r EQUAL 0)
      message(FATAL_ERROR "QuickJS patch ${_p} does not apply:\n${_e}")
    endif()
  endforeach()
  file(REMOVE_RECURSE ${_qjs_src}/.git)
  file(WRITE ${_qjs_stamp} "${_qjs_stamp_text}")
endif()
# quickjs-ng exports JS_FreeValue / JS_DupValue (and the RT variants) as
# out-of-line functions, so every value the interpreter pops or copies costs
# a call (12% of the instructions of a React commit). The patched copy in the
# build tree defines inline versions right after the refcount accessors and
# uses them throughout quickjs.c; the exported functions stay for embedders.
set(_qjs_c ${_qjs_src}/quickjs.c)
file(READ ${_qjs_c} _qjs)
set(_qjs_anchor "#define JS_GC_MARK(p)   (js_rc(p)->mark)\n")
set(_qjs_defs
  "void JS_FreeValueRT(JSRuntime *rt, JSValue v)\n{"
  "void JS_FreeValue(JSContext *ctx, JSValue v)\n{"
  "JSValue JS_DupValue(JSContext *ctx, JSValueConst v)\n{"
  "JSValue JS_DupValueRT(JSRuntime *rt, JSValueConst v)\n{")
set(_qjs_ok TRUE)
# jsui_qjs_patch(<name> <old1> <new1> [<old2> <new2> ...]): replaces each
# <old> in _qjs with its <new>. Every <old> must occur exactly once, or the
# whole patch is skipped with a warning. Use bracket arguments ([=[ ... ]=])
# so the C text needs no escaping.
set(JSUI_QUICKJS_PATCHES_OFF "" CACHE STRING
  "QuickJS hot-path patches to leave out (names from cmake/Deps.cmake), for bisecting")
function(jsui_qjs_patch name)
  if(name IN_LIST JSUI_QUICKJS_PATCHES_OFF)
    return()
  endif()
  math(EXPR _last "${ARGC} - 1")
  set(_src "${_qjs}")
  foreach(_i RANGE 1 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    string(FIND "${_src}" "${ARGV${_i}}" _first)
    string(FIND "${_src}" "${ARGV${_i}}" _lastpos REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _lastpos)
      message(WARNING "quickjs.c changed: building it without the ${name} patch")
      return()
    endif()
    string(REPLACE "${ARGV${_i}}" "${ARGV${_j}}" _src "${_src}")
  endforeach()
  set(_qjs "${_src}" PARENT_SCOPE)
endfunction()
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
/* Tags whose values can be strictly equal to each other share a class. */
jsui_inline int jsui_eq_class(int tag)
{
    switch (tag) {
    case JS_TAG_FLOAT64: return JS_TAG_INT;
    case JS_TAG_STRING_ROPE: return JS_TAG_STRING;
    case JS_TAG_BIG_INT: return JS_TAG_SHORT_BIG_INT;
    default: return tag;
    }
}
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
    else if (ta == JS_TAG_STRING && tb == JS_TAG_STRING)
        res = JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b) ||
              js_string_eq(JS_VALUE_GET_STRING(a), JS_VALUE_GET_STRING(b));
    else if (jsui_eq_class(ta) != jsui_eq_class(tb))
        res = false; /* e.g. a string and an object: never strictly equal */
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
  # ---- Further hot-path patches. Each one is all-or-nothing: if any of its
  # anchors is missing or not unique (upstream changed), it is skipped with a
  # warning and QuickJS stays correct, only slower.

  # Object.keys on an ordinary object with string keys only (props, style
  # and state objects): one pass over the shape into a fast array, instead
  # of collecting a sorted property table, re-checking each key and
  # defining each array element through JS_DefinePropertyValue.
  jsui_qjs_patch(object-keys
[=[static JSValue js_object_keys(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int kind)
{
]=]
[=[/* jsui: Object.keys() of a non-exotic object; JS_UNINITIALIZED when the
   object has integer-like keys (which sort first) or is not ordinary. */
static JSValue jsui_object_keys_fast(JSContext *ctx, JSObject *p)
{
    JSShape *sh = p->shape;
    JSShapeProperty *prs;
    JSValue r;
    JSObject *pr;
    uint32_t i, n = 0, val;

    if (p->is_exotic || p->fast_array)
        return JS_UNINITIALIZED;
    for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        JSAtom atom = prs->atom;
        JSAtomStruct *ap;
        if (atom == JS_ATOM_NULL || !(prs->flags & JS_PROP_ENUMERABLE))
            continue;
        if (__JS_AtomIsTaggedInt(atom))
            return JS_UNINITIALIZED;
        ap = ctx->rt->atom_array[atom];
        if (ap->atom_type != JS_ATOM_TYPE_STRING)
            continue; /* symbols and private names are not keys */
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF ||
            (is_num_string(&val, ap) && val != -1))
            return JS_UNINITIALIZED;
        n++;
    }
    r = js_allocate_fast_array(ctx, n);
    if (JS_IsException(r) || n == 0)
        return r;
    pr = JS_VALUE_GET_OBJ(r);
    n = 0;
    for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        JSAtom atom = prs->atom;
        JSAtomStruct *ap;
        if (atom == JS_ATOM_NULL || !(prs->flags & JS_PROP_ENUMERABLE))
            continue;
        ap = ctx->rt->atom_array[atom];
        if (ap->atom_type != JS_ATOM_TYPE_STRING)
            continue;
        pr->u.array.u.values[n++] = js_dup(JS_MKPTR(JS_TAG_STRING, ap));
    }
    return r;
}

static JSValue js_object_keys(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int kind)
{
    if (kind == JS_ITERATOR_KIND_KEY &&
        JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
        JSValue r = jsui_object_keys_fast(ctx, JS_VALUE_GET_OBJ(argv[0]));
        if (!JS_IsUninitialized(r))
            return r;
    }
]=])

  # Own property names of an ordinary object with string keys only (one pass
  # to count, one to fill, no per-key kind lookups through function calls).
  # Integer-like keys, which must be sorted first, take the general path.
  jsui_qjs_patch(own-names
[=[    /* clear pointer for consistency in case of failure */
    *ptab = NULL;
    *plen = 0;

    /* compute the number of returned properties */
]=]
[=[    /* clear pointer for consistency in case of failure */
    *ptab = NULL;
    *plen = 0;

    /* jsui: ordinary objects whose keys are all non-index strings */
    if (!p->is_exotic && !(flags & (JS_GPN_SYMBOL_MASK | JS_GPN_PRIVATE_MASK)) &&
        (flags & JS_GPN_STRING_MASK)) {
        uint32_t n = 0, val;
        sh = p->shape;
        for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
            JSAtomStruct *ap;
            atom = prs->atom;
            if (atom == JS_ATOM_NULL ||
                ((flags & JS_GPN_ENUM_ONLY) && !(prs->flags & JS_PROP_ENUMERABLE)))
                continue;
            if (__JS_AtomIsTaggedInt(atom))
                goto jsui_general;
            ap = ctx->rt->atom_array[atom];
            if (ap->atom_type != JS_ATOM_TYPE_STRING)
                continue;
            if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF ||
                (is_num_string(&val, ap) && val != -1))
                goto jsui_general;
            n++;
        }
        tab_atom = js_malloc(ctx, sizeof(tab_atom[0]) * max_int(n, 1));
        if (!tab_atom)
            return -1;
        j = 0;
        for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
            atom = prs->atom;
            if (atom == JS_ATOM_NULL ||
                ((flags & JS_GPN_ENUM_ONLY) && !(prs->flags & JS_PROP_ENUMERABLE)))
                continue;
            if (ctx->rt->atom_array[atom]->atom_type != JS_ATOM_TYPE_STRING)
                continue;
            tab_atom[j].atom = JS_DupAtom(ctx, atom);
            tab_atom[j].is_enumerable = (prs->flags & JS_PROP_ENUMERABLE) != 0;
            j++;
        }
        *ptab = tab_atom;
        *plen = n;
        return 0;
    }
 jsui_general:

    /* compute the number of returned properties */
]=])

  # for-in: prototypes without enumerable string keys (Object.prototype) are
  # recognised by a scan of their shape instead of a property-name table, and
  # the keys of the object itself are kept as a table in the iterator instead
  # of being defined one by one as properties of the iterator object.
  jsui_qjs_patch(for-in
[=[typedef struct JSForInIterator {
    JSValue obj;
    bool is_array;
    uint32_t array_length;
    uint32_t idx;
} JSForInIterator;]=]
[=[typedef struct JSForInIterator {
    JSValue obj;
    bool is_array;
    uint32_t array_length;
    uint32_t idx;
    JSPropertyEnum *tab; /* jsui: keys of the normal case, or NULL */
    uint32_t tab_count;
} JSForInIterator;]=]
[=[    JSForInIterator *it = p->u.for_in_iterator;
    JS_FreeValueRT(rt, it->obj);
    js_free_rt(rt, it);]=]
[=[    JSForInIterator *it = p->u.for_in_iterator;
    JS_FreeValueRT(rt, it->obj);
    if (it->tab) {
        uint32_t i;
        for (i = 0; i < it->tab_count; i++)
            JS_FreeAtomRT(rt, it->tab[i].atom);
        js_free_rt(rt, it->tab);
    }
    js_free_rt(rt, it);]=]
[=[    it->is_array = false;
    it->obj = obj;
    it->idx = 0;
    p = JS_VALUE_GET_OBJ(enum_obj);]=]
[=[    it->is_array = false;
    it->obj = obj;
    it->idx = 0;
    it->tab = NULL;
    it->tab_count = 0;
    p = JS_VALUE_GET_OBJ(enum_obj);]=]
[=[        if (JS_IsException(obj1))
            goto fail;
        if (JS_GetOwnPropertyNamesInternal(ctx, &tab_atom, &tab_atom_count,
                                           JS_VALUE_GET_OBJ(obj1),
                                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            JS_FreeValue(ctx, obj1);
            goto fail;
        }
        js_free_prop_enum(ctx, tab_atom, tab_atom_count);
        if (tab_atom_count != 0) {]=]
[=[        if (JS_IsException(obj1))
            goto fail;
        if (jsui_no_enumerable_string_keys(ctx, JS_VALUE_GET_OBJ(obj1))) {
            tab_atom_count = 0;
        } else if (JS_GetOwnPropertyNamesInternal(ctx, &tab_atom, &tab_atom_count,
                                                  JS_VALUE_GET_OBJ(obj1),
                                                  JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            JS_FreeValue(ctx, obj1);
            goto fail;
        } else {
            js_free_prop_enum(ctx, tab_atom, tab_atom_count);
        }
        if (tab_atom_count != 0) {]=]
[=[        for(i = 0; i < tab_atom_count; i++) {
            JS_SetPropertyInternal(ctx, enum_obj, tab_atom[i].atom, JS_NULL, 0);
        }
        js_free_prop_enum(ctx, tab_atom, tab_atom_count);
    }
    return enum_obj;]=]
[=[        it->tab = tab_atom;
        it->tab_count = tab_atom_count;
    }
    return enum_obj;]=]
[=[static JSValue build_for_in_iterator(JSContext *ctx, JSValue obj)
{]=]
[=[/* jsui: true when an ordinary object has no enumerable string-keyed own
   property, i.e. its JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY name list is empty. */
static bool jsui_no_enumerable_string_keys(JSContext *ctx, JSObject *p)
{
    JSShape *sh = p->shape;
    JSShapeProperty *prs;
    int i;
    if (p->is_exotic || p->fast_array)
        return false;
    for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        if (prs->atom != JS_ATOM_NULL && (prs->flags & JS_PROP_ENUMERABLE) &&
            JS_AtomGetKind(ctx, prs->atom) == JS_ATOM_KIND_STRING)
            return false;
    }
    return true;
}

static JSValue build_for_in_iterator(JSContext *ctx, JSValue obj)
{]=]
[=[        } else {
            JSShape *sh = p->shape;
            JSShapeProperty *prs;
            if (it->idx >= sh->prop_count)
                goto done;]=]
[=[        } else if (it->tab) {
            if (it->idx >= it->tab_count)
                goto done;
            prop = it->tab[it->idx++].atom;
        } else {
            JSShape *sh = p->shape;
            JSShapeProperty *prs;
            if (it->idx >= sh->prop_count)
                goto done;]=])

  # (Shape transitions that outlive the object that made them: the
  # react-native-quickjs patch 0051 does this now.)

  # Object literal fields ({a: x}) on a plain object that does not have the
  # property yet: add it straight to the shape. Anything else (existing key,
  # arrays, exotic or non-extensible objects) takes JS_DefinePropertyValue.
  jsui_qjs_patch(define-field
[=[                ret = JS_DefinePropertyValue(ctx, sp[-2], atom, sp[-1],
                                             JS_PROP_C_W_E | JS_PROP_THROW);
                sp--;
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;

        CASE(OP_set_name):]=]
[=[                {
                    JSObject *p = JS_VALUE_GET_OBJ(sp[-2]);
                    JSProperty *pr;
                    if (likely(JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_OBJECT &&
                               !p->is_exotic && !p->fast_array && p->extensible &&
                               !find_own_property(&pr, p, atom))) {
#ifdef JSUI_PROP_STATIC_KEY /* the shape-transitions patch */
                        pr = add_property2(ctx, p, atom, JS_PROP_C_W_E, true);
#else
                        pr = add_property(ctx, p, atom, JS_PROP_C_W_E);
#endif
                        if (unlikely(!pr))
                            goto exception;
                        pr->u.value = sp[-1];
                        sp--;
                        BREAK;
                    }
                }
                ret = JS_DefinePropertyValue(ctx, sp[-2], atom, sp[-1],
                                             JS_PROP_C_W_E | JS_PROP_THROW);
                sp--;
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;

        CASE(OP_set_name):]=])
  set(_qjs_c ${CMAKE_BINARY_DIR}/quickjs-jsui/quickjs.c)
  file(CONFIGURE OUTPUT ${_qjs_c} CONTENT "${_qjs}" @ONLY)
else()
  message(WARNING "quickjs.c changed: building it without the inline refcount patch")
endif()
add_library(jsui_quickjs STATIC
  ${_qjs_src}/dtoa.c
  ${_qjs_src}/libregexp.c
  ${_qjs_src}/libunicode.c
  ${_qjs_c})
target_include_directories(jsui_quickjs PUBLIC ${_qjs_src} ${quickjs_SOURCE_DIR})
# The debugger hooks of the engine patches (0003-0005): compiled only into
# builds with JSUI_DEBUGGER; without them the parser never emits a trap.
if(JSUI_DEBUGGER)
  target_compile_definitions(jsui_quickjs PUBLIC JS_ENABLE_DEBUGGER=1)
  # The Chrome DevTools Protocol agent (vendored from react-native-quickjs).
  target_sources(jsui_quickjs PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../third_party/quickjs-cdp/quickjs-cdp.c)
  target_include_directories(jsui_quickjs PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../third_party/quickjs-cdp)
  find_package(Threads REQUIRED)
  target_link_libraries(jsui_quickjs PUBLIC Threads::Threads)
else()
  target_compile_definitions(jsui_quickjs PUBLIC JS_ENABLE_DEBUGGER=0)
endif()
target_compile_definitions(jsui_quickjs PRIVATE _GNU_SOURCE)
# Extra compile flags for QuickJS alone.
set(JSUI_QUICKJS_FLAGS "" CACHE STRING "Extra compile flags for QuickJS")
if(JSUI_QUICKJS_FLAGS)
  separate_arguments(_qjs_flags NATIVE_COMMAND "${JSUI_QUICKJS_FLAGS}")
  target_compile_options(jsui_quickjs PRIVATE ${_qjs_flags})
endif()
# Profile-guided optimization of QuickJS (JSUI_QUICKJS_PGO /
# JSUI_QUICKJS_PROFILE, scripts/pgo-quickjs.sh). Off unless asked for.
include(QuickJSPGO)
if(UNIX)
  target_link_libraries(jsui_quickjs PUBLIC m)
endif()
set_target_properties(jsui_quickjs PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jsui_quickjs PRIVATE -w)
endif()

# ---------------------------------------------------------------------- Yoga
FetchContent_Declare(yoga
  GIT_REPOSITORY https://github.com/facebook/yoga.git
  GIT_TAG v3.2.1
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(yoga)
if(NOT yoga_POPULATED)
  FetchContent_Populate(yoga)
endif()
file(GLOB_RECURSE JSUI_YOGA_SOURCES CONFIGURE_DEPENDS ${yoga_SOURCE_DIR}/yoga/*.cpp)
# JSUI keeps layout fractional (point scale factor 0), but Yoga still walks
# the whole tree after every layout pass to "round" it. Compile a copy of
# PixelGrid.cpp that returns at once when there is nothing to round (the
# fetched sources stay untouched).
set(_yoga_pixel_grid ${yoga_SOURCE_DIR}/yoga/algorithm/PixelGrid.cpp)
file(READ ${_yoga_pixel_grid} _yoga_pg)
set(_yoga_pg_anchor "  const auto pointScaleFactor = node->getConfig()->getPointScaleFactor();\n")
string(FIND "${_yoga_pg}" "${_yoga_pg_anchor}" _yoga_pg_at)
if(_yoga_pg_at GREATER -1)
  string(REPLACE "${_yoga_pg_anchor}"
    "${_yoga_pg_anchor}  if (pointScaleFactor == 0.0f) return;  // jsui: fractional layout, nothing to round\n"
    _yoga_pg "${_yoga_pg}")
  set(_yoga_pg_out ${CMAKE_BINARY_DIR}/yoga-patched/PixelGrid.cpp)
  file(CONFIGURE OUTPUT ${_yoga_pg_out} CONTENT "${_yoga_pg}" @ONLY)
  list(REMOVE_ITEM JSUI_YOGA_SOURCES ${_yoga_pixel_grid})
  list(APPEND JSUI_YOGA_SOURCES ${_yoga_pg_out})
else()
  message(WARNING "Yoga PixelGrid.cpp changed: skipping the no-rounding fast path")
endif()
# justifyMainAxis asks isBaselineLayout(node) once per flex line, and that
# scans every child: O(lines x children) for a wrapping grid. Remember the
# answer for the node within one layout pass (styles cannot change during
# a pass; the generation count changes with every YGNodeCalculateLayout).
set(_yoga_calc ${yoga_SOURCE_DIR}/yoga/algorithm/CalculateLayout.cpp)
file(READ ${_yoga_calc} _yoga_cl)
set(_yoga_cl_anchor "  bool isNodeBaselineLayout = isBaselineLayout(node);\n")
string(FIND "${_yoga_cl}" "${_yoga_cl_anchor}" _yoga_cl_at)
if(_yoga_cl_at GREATER -1)
  string(REPLACE "${_yoga_cl_anchor}" "\
  // jsui: once per node and layout pass instead of once per flex line.\n\
  static thread_local const yoga::Node* jsuiBaselineNode = nullptr;\n\
  static thread_local uint32_t jsuiBaselineGen = 0;\n\
  static thread_local bool jsuiBaselineValue = false;\n\
  const uint32_t jsuiGen = gCurrentGenerationCount.load(std::memory_order_relaxed);\n\
  if (node != jsuiBaselineNode || jsuiGen != jsuiBaselineGen) {\n\
    jsuiBaselineNode = node;\n\
    jsuiBaselineGen = jsuiGen;\n\
    jsuiBaselineValue = isBaselineLayout(node);\n\
  }\n\
  bool isNodeBaselineLayout = jsuiBaselineValue;\n" _yoga_cl "${_yoga_cl}")
  set(_yoga_cl_out ${CMAKE_BINARY_DIR}/yoga-patched/CalculateLayout.cpp)
  file(CONFIGURE OUTPUT ${_yoga_cl_out} CONTENT "${_yoga_cl}" @ONLY)
  list(REMOVE_ITEM JSUI_YOGA_SOURCES ${_yoga_calc})
  list(APPEND JSUI_YOGA_SOURCES ${_yoga_cl_out})
else()
  message(WARNING "Yoga CalculateLayout.cpp changed: skipping the baseline-layout memo")
endif()
# Browser parity for `flex: N` items in containers sized by their content.
# react-native-web turns `flex: 1` into `flex: 1 1 0%`, and a 0% basis has
# nothing definite to resolve against while its container is being sized to
# fit its content, so browsers size the item by its content. Yoga used a 0
# basis there and never grew it: `flex: 1` text in a row inside an
# alignItems: 'flex-start' column was measured at zero width, one character
# per line (thousands of pixels tall). Patched into the same CalculateLayout.cpp
# copy; these are correctness fixes, so a missing anchor (a Yoga update) is an
# error, not a warning. See tests/native/style_test.cpp and examples/layout/.
if(NOT DEFINED _yoga_cl_out)
  set(_yoga_cl_out ${CMAKE_BINARY_DIR}/yoga-patched/CalculateLayout.cpp)
  list(REMOVE_ITEM JSUI_YOGA_SOURCES ${_yoga_calc})
  list(APPEND JSUI_YOGA_SOURCES ${_yoga_cl_out})
endif()
function(jsui_yoga_patch anchor replacement)
  string(FIND "${_yoga_cl}" "${anchor}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "Yoga CalculateLayout.cpp changed: flex-basis patch anchor not found:\n${anchor}")
  endif()
  string(REPLACE "${anchor}" "${replacement}" _yoga_cl "${_yoga_cl}")
  set(_yoga_cl "${_yoga_cl}" PARENT_SCOPE)
endfunction()
# 1. While the container's main axis is sized by content, a basis from the
#    `flex` shorthand is content-based (and recomputed on every pass, since an
#    earlier pass may have cached a content basis).
jsui_yoga_patch("\
  const FloatOptional resolvedFlexBasis = child->resolveFlexBasis(
      direction, mainAxis, mainAxisOwnerSize, ownerWidth);
" "\
  FloatOptional resolvedFlexBasis = child->resolveFlexBasis(
      direction, mainAxis, mainAxisOwnerSize, ownerWidth);
  // jsui: browser parity, see cmake/Deps.cmake.
  const auto jsuiBasisUnit = child->style().flexBasis().unit();
  const bool jsuiShorthandBasis =
      (jsuiBasisUnit == Unit::Auto || jsuiBasisUnit == Unit::Undefined) &&
      child->style().flex().isDefined() && child->style().flex().unwrap() > 0.0f;
  if (jsuiShorthandBasis &&
      (isMainAxisRow ? widthMode : heightMode) != SizingMode::StretchFit) {
    resolvedFlexBasis = FloatOptional{};
  }
")
jsui_yoga_patch("\
    if (child->getLayout().computedFlexBasis.isUndefined() ||
        (child->getConfig()->isExperimentalFeatureEnabled(" "\
    if (child->getLayout().computedFlexBasis.isUndefined() ||
        jsuiShorthandBasis ||
        (child->getConfig()->isExperimentalFeatureEnabled(")
# 2. A fit-content row is min(max-content, available): when its content
#    overflows the available width and something can shrink, lay it out at
#    that width instead of at its content size. Rows only: CSS heights are
#    never fit-content. The flex step then cannot be skipped, since the shrunk
#    sizes are the measurement, so justifyMainAxis takes the caller's decision.
jsui_yoga_patch("\
    const bool canSkipFlex =
        !performLayout && sizingModeCrossDim == SizingMode::StretchFit;
" "\
    bool canSkipFlex =
        !performLayout && sizingModeCrossDim == SizingMode::StretchFit;
    const float jsuiAvailableInnerMainDim = availableInnerMainDim;
")
jsui_yoga_patch("\
        sizeBasedOnContent = !useLegacyStretchBehaviour;
" "\
        sizeBasedOnContent = !useLegacyStretchBehaviour;
        // jsui: browser parity, see cmake/Deps.cmake.
        if (isMainAxisRow && sizingModeMainDim == SizingMode::FitContent &&
            yoga::isDefined(jsuiAvailableInnerMainDim) &&
            flexLine.sizeConsumed > jsuiAvailableInnerMainDim &&
            flexLine.layout.totalFlexShrinkScaledFactors != 0) {
          availableInnerMainDim = jsuiAvailableInnerMainDim;
          sizeBasedOnContent = false;
          canSkipFlex = false;
        }
")
jsui_yoga_patch("\
    const float availableInnerWidth,
    const bool performLayout) {
  const auto& style = node->style();
" "\
    const float availableInnerWidth,
    const bool performLayout,
    const bool canSkipFlex) {
  const auto& style = node->style();
")
jsui_yoga_patch("\
    bool canSkipFlex =
        !performLayout && sizingModeCrossDim == SizingMode::StretchFit;
    if (canSkipFlex) {" "\
    (void)sizingModeCrossDim;  // jsui: canSkipFlex comes from the caller
    if (canSkipFlex) {")
jsui_yoga_patch("\
        availableInnerWidth,
        performLayout);

    float containerCrossAxis" "\
        availableInnerWidth,
        performLayout,
        canSkipFlex);

    float containerCrossAxis")
file(CONFIGURE OUTPUT ${_yoga_cl_out} CONTENT "${_yoga_cl}" @ONLY)

add_library(jsui_yoga STATIC ${JSUI_YOGA_SOURCES})
target_include_directories(jsui_yoga PUBLIC ${yoga_SOURCE_DIR})
set_target_properties(jsui_yoga PROPERTIES CXX_STANDARD 20 POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------- Skia
set(JSUI_SKIA_DIR "" CACHE PATH "Directory with a Skia build (include/, modules/, out/<lib dir>)")
set(JSUI_SKIA_LIB_DIR "" CACHE PATH "Directory with Skia static libraries")

if(NOT JSUI_SKIA_DIR)
  if(EMSCRIPTEN)
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-wasm)
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      message(FATAL_ERROR "WASM Skia not found at ${JSUI_SKIA_DIR}. Run scripts/build-skia-wasm.sh first.")
    endif()
  elseif(IOS)
    # Built from source by scripts/build-skia-ios.sh (no prebuilt for iOS):
    # one library slice per SDK + architecture, picked from the toolchain.
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-ios)
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      message(FATAL_ERROR "iOS Skia not found at ${JSUI_SKIA_DIR}. Run scripts/build-skia-ios.sh first.")
    endif()
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      set(_ios_sdk iphonesimulator)
    else()
      set(_ios_sdk iphoneos)
    endif()
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
      set(_ios_cpu x64)
    else()
      set(_ios_cpu arm64)
    endif()
    if(NOT JSUI_SKIA_LIB_DIR)
      set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out/${_ios_sdk}-${_ios_cpu})
    endif()
    if(NOT EXISTS ${JSUI_SKIA_LIB_DIR}/libskia.a)
      message(FATAL_ERROR "No iOS Skia slice at ${JSUI_SKIA_LIB_DIR}: SLICES=${_ios_sdk}-${_ios_cpu} scripts/build-skia-ios.sh")
    endif()
  elseif(ANDROID)
    # Built from source by scripts/build-skia-android.sh (no prebuilt for
    # Android): one library dir per ABI.
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-android)
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      message(FATAL_ERROR "Android Skia not found at ${JSUI_SKIA_DIR}. Run scripts/build-skia-android.sh first.")
    endif()
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
      set(_android_cpu x64)
    else()
      set(_android_cpu arm64)
    endif()
    if(NOT JSUI_SKIA_LIB_DIR)
      set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out/android-${_android_cpu})
    endif()
    if(NOT EXISTS ${JSUI_SKIA_LIB_DIR}/libskia.a)
      message(FATAL_ERROR "No Android Skia at ${JSUI_SKIA_LIB_DIR}: CPUS=${_android_cpu} scripts/build-skia-android.sh")
    endif()
  else()
    if(APPLE)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_skia_pkg Skia-macOS-Release-arm64)
        set(_skia_out Release-arm64)
      else()
        set(_skia_pkg Skia-macOS-Release-x64)
        set(_skia_out Release-x64)
      endif()
    elseif(WIN32)
      set(_skia_pkg Skia-Windows-Release-x64)
      set(_skia_out Release-x64)
    else()
      set(_skia_pkg Skia-Linux-Release-x64)
      set(_skia_out Release-x64)
    endif()
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-prebuilt/${_skia_pkg})
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      set(_url https://github.com/aseprite/skia/releases/download/m124-08a5439a6b/${_skia_pkg}.zip)
      message(STATUS "Downloading prebuilt Skia: ${_url}")
      file(DOWNLOAD ${_url} ${CMAKE_BINARY_DIR}/${_skia_pkg}.zip SHOW_PROGRESS STATUS _st)
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        message(FATAL_ERROR "Skia download failed: ${_st}")
      endif()
      file(MAKE_DIRECTORY ${JSUI_SKIA_DIR})
      file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/${_skia_pkg}.zip DESTINATION ${JSUI_SKIA_DIR})
    endif()
    if(NOT JSUI_SKIA_LIB_DIR)
      set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out/${_skia_out})
    endif()
  endif()
endif()
# A desktop Skia built from source by scripts/build-skia-desktop.sh
# (-DJSUI_SKIA_DIR=third_party/skia-desktop): one library dir per OS + CPU.
if(NOT JSUI_SKIA_LIB_DIR AND NOT EMSCRIPTEN AND NOT IOS)
  if(APPLE)
    set(_skia_desktop_os macos)
  elseif(WIN32)
    set(_skia_desktop_os win)
  else()
    set(_skia_desktop_os linux)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(_skia_desktop_cpu arm64)
  else()
    set(_skia_desktop_cpu x64)
  endif()
  if(EXISTS ${JSUI_SKIA_DIR}/out/${_skia_desktop_os}-${_skia_desktop_cpu}/libskia.a OR
     EXISTS ${JSUI_SKIA_DIR}/out/${_skia_desktop_os}-${_skia_desktop_cpu}/skia.lib)
    set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out/${_skia_desktop_os}-${_skia_desktop_cpu})
  endif()
endif()
if(NOT JSUI_SKIA_LIB_DIR)
  set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out)
endif()
# Source-built native Skia (desktop or iOS): it records the defines its public
# headers need (SK_GANESH, SK_GRAPHITE, SK_VULKAN, SK_METAL, SK_SHAPER_*...)
# and its library set differs from the prebuilt's (m157: skunicode_core /
# _icu, jpeg12 / 16, ...), so every library of the build is linked.
set(JSUI_SKIA_SOURCE_BUILT OFF)
if(NOT EMSCRIPTEN AND EXISTS "${JSUI_SKIA_LIB_DIR}/defines.txt")
  set(JSUI_SKIA_SOURCE_BUILT ON)
endif()
message(STATUS "Skia: ${JSUI_SKIA_LIB_DIR}")

add_library(jsui_skia INTERFACE)
target_include_directories(jsui_skia INTERFACE ${JSUI_SKIA_DIR})
if(WIN32)
  set(_p "")
  set(_s ".lib")
else()
  set(_p "lib")
  set(_s ".a")
endif()
# Order matters for single-pass linkers: dependents before dependencies.
# (iOS links every library of its Skia slice, below.)
if(JSUI_SKIA_SOURCE_BUILT)
  set(_jsui_skia_named_libs "")
else()
  set(_jsui_skia_named_libs skparagraph skshaper skunicode skresources skia harfbuzz icu freetype2 png jpeg webp webp_sse41 wuffs zlib expat skcms)
endif()
# Lottie (LottieView): Skia's Skottie module, when the Skia build has it
# (the aseprite prebuilt and every scripts/build-skia-*.sh build do).
# Off, LottieView sources fail with an error and nothing Skottie is linked.
set(_jsui_lottie_default OFF)
if(EXISTS ${JSUI_SKIA_LIB_DIR}/${_p}skottie${_s})
  set(_jsui_lottie_default ON)
endif()
option(JSUI_LOTTIE "Lottie animations (LottieView) with Skia's Skottie module" ${_jsui_lottie_default})
if(JSUI_LOTTIE AND NOT JSUI_SKIA_SOURCE_BUILT)
  list(PREPEND _jsui_skia_named_libs skottie sksg jsonreader)
endif()
foreach(_lib ${_jsui_skia_named_libs})
  set(_f ${JSUI_SKIA_LIB_DIR}/${_p}${_lib}${_s})
  if(EXISTS ${_f})
    target_link_libraries(jsui_skia INTERFACE ${_f})
  endif()
endforeach()
target_compile_definitions(jsui_skia INTERFACE SK_RELEASE)
if(JSUI_SKIA_SOURCE_BUILT AND NOT IOS)
  file(STRINGS "${JSUI_SKIA_LIB_DIR}/defines.txt" _jsui_skia_defs)
  target_compile_definitions(jsui_skia INTERFACE ${_jsui_skia_defs})
  file(GLOB _jsui_skia_libs "${JSUI_SKIA_LIB_DIR}/lib*.a")
  if(WIN32)
    # link.exe / lld-link resolve archives regardless of order.
    file(GLOB _jsui_skia_libs "${JSUI_SKIA_LIB_DIR}/*.lib")
    target_link_libraries(jsui_skia INTERFACE ${_jsui_skia_libs})
  elseif(APPLE)
    target_link_libraries(jsui_skia INTERFACE ${_jsui_skia_libs})
  else()
    # GNU ld resolves archives in order: one group for the whole set.
    target_link_libraries(jsui_skia INTERFACE "-Wl,--start-group" ${_jsui_skia_libs} "-Wl,--end-group")
  endif()
endif()
if(ANDROID)
  # Ganesh GL through EGL / GLES; the Android font manager reads
  # /system/etc/fonts.xml (bundled FreeType + expat).
  target_link_libraries(jsui_skia INTERFACE EGL GLESv2 log android ${CMAKE_DL_LIBS})
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(Fontconfig QUIET)
  find_package(Threads REQUIRED)
  target_link_libraries(jsui_skia INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
  if(Fontconfig_FOUND)
    target_link_libraries(jsui_skia INTERFACE Fontconfig::Fontconfig)
  endif()
elseif(IOS)
  # The source-built Skia records the defines its public headers need
  # (SK_GANESH, SK_METAL, SK_SHAPER_*...); compile with exactly those.
  if(EXISTS "${JSUI_SKIA_LIB_DIR}/defines.txt")
    file(STRINGS "${JSUI_SKIA_LIB_DIR}/defines.txt" _jsui_skia_defs)
    target_compile_definitions(jsui_skia INTERFACE ${_jsui_skia_defs})
  endif()
  # Every library of the slice (m157 adds skunicode_core / _icu, jpeg12 /
  # 16, ...); ld64 resolves archives regardless of order.
  file(GLOB _jsui_skia_libs "${JSUI_SKIA_LIB_DIR}/lib*.a")
  target_link_libraries(jsui_skia INTERFACE ${_jsui_skia_libs})
  target_link_libraries(jsui_skia INTERFACE "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText"
    "-framework Foundation" "-framework UIKit" "-framework Metal")
elseif(APPLE)
  target_link_libraries(jsui_skia INTERFACE "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText" "-framework CoreServices")
  if(JSUI_SKIA_SOURCE_BUILT)
    # skia_use_metal (Ganesh + Graphite on Metal); ImageIO for the platform codecs.
    target_link_libraries(jsui_skia INTERFACE "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework ImageIO")
  endif()
elseif(WIN32)
  target_link_libraries(jsui_skia INTERFACE user32 gdi32 ole32 opengl32 usp10)
  if(JSUI_SKIA_SOURCE_BUILT)
    # DirectWrite fonts (FontSub, OleAut32), ICU's registry lookups (Advapi32)
    # and, with skia_use_direct3d, Ganesh's Direct3D 12.
    target_link_libraries(jsui_skia INTERFACE oleaut32 fontsub advapi32)
    file(READ "${JSUI_SKIA_LIB_DIR}/args.gn" _jsui_skia_win_gn)
    if(_jsui_skia_win_gn MATCHES "skia_use_direct3d *= *true")
      target_link_libraries(jsui_skia INTERFACE d3d12 dxgi d3dcompiler)
    endif()
  endif()
endif()

# On Windows the prebuilt Skia's ICU loads its data from icudtl.dat next to
# the executable (SkLoadICU); without it SkUnicode is unavailable and text
# shaping fails. Embedders ship the same file beside their game binary.
set(JSUI_ICU_DATA "")
if(WIN32 AND EXISTS ${JSUI_SKIA_DIR}/third_party/externals/icu/flutter/icudtl.dat)
  set(JSUI_ICU_DATA ${JSUI_SKIA_DIR}/third_party/externals/icu/flutter/icudtl.dat)
endif()
function(jsui_copy_icu_data target)
  if(JSUI_ICU_DATA)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JSUI_ICU_DATA} $<TARGET_FILE_DIR:${target}>/icudtl.dat)
  endif()
endfunction()

# The prebuilt Skia's FreeType (ftgzip) needs zlib's unprefixed inflate*
# symbols. Use the system zlib where there is one (Linux, macOS SDK) and
# build it from source elsewhere (Windows). A source-built Skia
# (scripts/build-skia-desktop.sh) links its own zlib and has no FreeType on
# Windows: nothing to add there.
if(NOT EMSCRIPTEN AND NOT (WIN32 AND JSUI_SKIA_SOURCE_BUILT))
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_link_libraries(jsui_skia INTERFACE ZLIB::ZLIB)
  else()
    FetchContent_Declare(zlib
      GIT_REPOSITORY https://github.com/madler/zlib.git
      GIT_TAG v1.3.1
      GIT_SHALLOW TRUE)
    FetchContent_GetProperties(zlib)
    if(NOT zlib_POPULATED)
      FetchContent_Populate(zlib)
    endif()
    # Only the codec is needed; the gz* file API wants platform headers.
    file(GLOB JSUI_ZLIB_SOURCES ${zlib_SOURCE_DIR}/*.c)
    list(FILTER JSUI_ZLIB_SOURCES EXCLUDE REGEX "/gz[a-z]+\\.c$")
    add_library(jsui_zlib STATIC ${JSUI_ZLIB_SOURCES})
    target_include_directories(jsui_zlib PUBLIC ${zlib_SOURCE_DIR})
    if(MSVC)
      target_compile_definitions(jsui_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
    endif()
    target_link_libraries(jsui_skia INTERFACE jsui_zlib)
  endif()
endif()

# ----------------------------------------------------------------- miniaudio
# The built-in audio backend (JSUI_AUDIO, src/audio/audio_builtin.cpp):
# device output, mixing and decoding (WAV, FLAC, MP3, and Ogg Vorbis through
# the stb_vorbis copy miniaudio ships in extras/). One C translation unit,
# src/audio/miniaudio.c. The MA_* options are PUBLIC: they change miniaudio's
# structs, so the backend must see the same ones.
if(JSUI_AUDIO)
  FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(miniaudio)
  if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
  endif()
  add_library(jsui_miniaudio STATIC ${CMAKE_SOURCE_DIR}/src/audio/miniaudio.c)
  target_include_directories(jsui_miniaudio PUBLIC ${miniaudio_SOURCE_DIR})
  # Not used by the backend: the resource manager (sounds are decoded by the
  # backend from the loader's bytes), encoders and waveform / noise generators.
  target_compile_definitions(jsui_miniaudio PUBLIC MA_NO_RESOURCE_MANAGER MA_NO_ENCODING MA_NO_GENERATION)
  set_target_properties(jsui_miniaudio PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(jsui_miniaudio PRIVATE -w)
  elseif(MSVC)
    target_compile_options(jsui_miniaudio PRIVATE /w)
  endif()
  if(APPLE)
    # Linked, not loaded at run time (miniaudio's default there), which
    # Apple's notarization does not accept without extra entitlements.
    target_compile_definitions(jsui_miniaudio PUBLIC MA_NO_RUNTIME_LINKING)
    target_link_libraries(jsui_miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  elseif(UNIX)
    # ALSA / PulseAudio / JACK are loaded at run time (dlopen): nothing to
    # link against, no build dependency on their headers.
    find_package(Threads REQUIRED)
    target_link_libraries(jsui_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
  endif()
endif()
