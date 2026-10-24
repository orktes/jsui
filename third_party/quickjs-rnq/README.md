# react-native-quickjs engine patches, ported to quickjs-ng v0.17.0

WIP. These are the 72 engine patches from react-native-quickjs
(https://github.com/react-native-quickjs/quickjs, commit c8102d4, MIT, see
LICENSE), rebased from their base (quickjs-ng df836d1, v0.16.2+17) onto
v0.17.0 with `git rebase` (3-way):

- 0006 (JS_NewPrivateSymbol) is already in v0.17.0 and is dropped.
- 0061 and 0064 needed small conflict fixes (0064: JS_GetPropertyValue is a
  public function in v0.17.0, so the wrapper stays non-static).
- 9999 bumps BC_VERSION to 29 (v0.17.0 is already at 28), regenerates the
  builtin bytecode headers and gen/*.c with the patched qjsc, and updates the
  test_bjson.js fuzz corpus version byte.

Apply in order with `git am` (or `git apply`) on a v0.17.0 checkout. What each
patch does and why: UPSTREAM-README.md. With this series the quickjs-ng
tests pass like stock v0.17.0 (test_builtin's `qjs` global and
test_worker_module fail the same way on stock when run as plain scripts).
