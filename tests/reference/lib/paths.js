// Well-known locations, resolved relative to this package so the harness
// works from any cwd (npm workspaces run scripts from the package dir).
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

export const PKG_DIR = path.resolve(here, '..');
export const REPO_ROOT = path.resolve(PKG_DIR, '../..');
export const EXAMPLES_DIR = path.join(REPO_ROOT, 'examples');
export const FONTS_DIR = path.join(REPO_ROOT, 'assets/fonts');
export const OUTPUT_DIR = process.env.JSUI_REF_OUTPUT
  ? path.resolve(process.env.JSUI_REF_OUTPUT)
  : path.join(PKG_DIR, 'output');
export const CACHE_DIR = path.join(OUTPUT_DIR, '.cache');
export const JSUI_RENDER = process.env.JSUI_RENDER
  ? path.resolve(process.env.JSUI_RENDER)
  : path.join(REPO_ROOT, 'build/jsui-render');

// Inter faces shipped in assets/fonts, shared by both sides of the comparison.
export const INTER_FACES = [
  { file: 'Inter-Regular.ttf', weight: 400, style: 'normal' },
  { file: 'Inter-Medium.ttf', weight: 500, style: 'normal' },
  { file: 'Inter-SemiBold.ttf', weight: 600, style: 'normal' },
  { file: 'Inter-Bold.ttf', weight: 700, style: 'normal' },
  { file: 'Inter-Italic.ttf', weight: 400, style: 'italic' },
];
