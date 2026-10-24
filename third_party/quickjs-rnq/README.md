# react-native-quickjs engine patches, on quickjs-ng v0.17.0

The engine patches of react-native-quickjs
(https://github.com/react-native-quickjs/quickjs at c8102d4, MIT, see
LICENSE), with their own headers. What each does and why: UPSTREAM-README.md
(their table, unchanged). `cmake/Deps.cmake` applies them in file name order
to a copy of the fetched v0.17.0 sources, then `third_party/quickjs-jsui`.

They were written against quickjs-ng df836d1 (v0.16.2+17) and rebased onto
v0.17.0 with a 3-way `git rebase`:

- 0006 (`JS_NewPrivateSymbol`) is in v0.17.0 already and is left out.
- 0061 and 0064 needed small conflict fixes (0064: `JS_GetPropertyValue`
  is public in v0.17.0, so its wrapper stays an exported function).
- 9999 raises the bytecode version to 29 (v0.17.0 is at 28 already).

With the series, quickjs-ng's test262 config (`run-test262 -c tests.conf`)
and tests/*.js pass as on stock v0.17.0.

To update: apply the series on a v0.17.0 checkout as commits, rebase them
onto the new quickjs-ng with `git rebase`, and regenerate the files with
`git format-patch`, keeping these headers.
