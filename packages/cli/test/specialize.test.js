import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { HOST_FLAGS, specializeReconciler } from '../src/specialize.js';
import { jsuiReactDir } from '../src/config.js';

const require = createRequire(import.meta.url);

test('specialize: every patch applies to the installed react-reconciler', async () => {
  const file = require.resolve('react-reconciler/cjs/react-reconciler.production.js', { paths: [jsuiReactDir()] });
  const { applied, failed } = specializeReconciler(await readFile(file, 'utf8'));
  assert.deepEqual(failed, []);
  assert.ok(applied.length > 0);
});

test('specialize: what it folds matches the host config', async () => {
  const { hostConfig } = await import('../../react/src/renderer.js');
  for (const [k, v] of Object.entries(HOST_FLAGS)) assert.equal(hostConfig[k], v, k);
  assert.equal(hostConfig.shouldSetTextContent('Text', { children: 'x' }), false);
  const inst = {};
  assert.equal(hostConfig.getPublicInstance(inst), inst);
  assert.equal(hostConfig.createFragmentInstance({}), null);
});
