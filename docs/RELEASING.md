# Releasing the npm packages

Three packages are published under the `@orktes` npm scope, always with the
same version:

| Package       | Directory        | Contents                                         |
| ------------- | ---------------- | ------------------------------------------------ |
| `@orktes/jsui-react` | `packages/react` | React Native API on the JSUI runtime             |
| `@orktes/jsui-cli`   | `packages/cli`   | `jsui build` / `jsui dev` (esbuild)              |
| `@orktes/jsui-web`   | `web`            | browser host + the three WASM modules            |

`.github/workflows/release.yml` publishes them with npm trusted publishing
(GitHub OIDC, with provenance; there is no npm token in the repository). It
does not compile anything: the WASM modules come from the CI run on `main`
for the released commit (artifacts `jsui-wasm`, `jsui-wasm-webgl`,
`jsui-wasm-gpu`), the same ones the Pages deploy uses, so that run must have
succeeded with all three, including the advisory WebGPU job.

## Cutting a release

1. Bump the version in lockstep: `node scripts/set-version.mjs 0.2.0`
   (updates the three `package.json`s and `@orktes/jsui-cli`'s dependency on
   `@orktes/jsui-react`). Merge that to `main` and wait for CI to pass.
2. Tag the merge commit and push the tag: `git tag v0.2.0 && git push origin v0.2.0`.
   (Or run the Release workflow by hand from the Actions tab on `main`.)
3. The workflow checks the tag matches the package versions, downloads the
   WASM modules, runs the JS tests, and publishes `@orktes/jsui-react`, then
   `@orktes/jsui-web`, then `@orktes/jsui-cli` (which depends on both). Versions already on npm are skipped, so a
   failed run can simply be re-run.

Prerelease versions (`0.2.0-rc.1`) are published under the `next` dist-tag;
everything else goes to `latest`.

## Trusted publisher setup (once per package)

npm only lets a package that already exists trust a workflow, so the first
version of each package was published by hand (`npm publish --workspace …`
after downloading the CI artifacts into `web/dist*`). Then, for each of the
three packages on npmjs.com: Settings → Trusted publishing → GitHub Actions,
with organization/user `orktes`, repository `jsui`, workflow filename
`release.yml`, no environment. After that, Settings → Publishing access →
"Require two-factor authentication and disallow tokens" blocks any other way
of publishing.
