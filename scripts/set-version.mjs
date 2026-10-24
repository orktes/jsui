#!/usr/bin/env node
// Sets the version of the published npm packages (@orktes/jsui-react, @orktes/jsui-cli,
// @orktes/jsui-web) in lockstep, including @orktes/jsui-cli's dependency on @orktes/jsui-react and
// the workspaces that depend on them (site/) and the CLI's Claude Code plugin manifest.  Usage: node scripts/set-version.mjs 0.2.0
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const version = process.argv[2];
if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(version ?? '')) {
  console.error('usage: node scripts/set-version.mjs <semver>');
  process.exit(1);
}
const PUBLISHED = ['packages/react', 'packages/cli', 'web'];
const names = new Set(PUBLISHED.map((d) => JSON.parse(fs.readFileSync(path.join(ROOT, d, 'package.json'), 'utf8')).name));
for (const dir of [...PUBLISHED, 'site']) {
  const file = path.join(ROOT, dir, 'package.json');
  if (!fs.existsSync(file)) continue;
  const pkg = JSON.parse(fs.readFileSync(file, 'utf8'));
  if (PUBLISHED.includes(dir)) pkg.version = version;
  for (const field of ['dependencies', 'devDependencies', 'peerDependencies']) {
    for (const dep of Object.keys(pkg[field] ?? {})) if (names.has(dep)) pkg[field][dep] = version;
  }
  fs.writeFileSync(file, JSON.stringify(pkg, null, 2) + '\n');
  console.log(`${dir}/package.json`);
}
// The Claude Code plugin that ships the CLI's agent skill (packages/cli/.claude-plugin).
const plugin = path.join(ROOT, 'packages/cli/.claude-plugin/plugin.json');
if (fs.existsSync(plugin)) {
  const manifest = JSON.parse(fs.readFileSync(plugin, 'utf8'));
  manifest.version = version;
  fs.writeFileSync(plugin, JSON.stringify(manifest, null, 2) + '\n');
  console.log('packages/cli/.claude-plugin/plugin.json');
}
console.log('Now run npm install to refresh package-lock.json.');
