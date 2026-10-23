// `jsui skill`: the agent skill (SKILL.md + references) that ships with this
// package, so coding agents learn JSUI at the version they build with.

import { cp, mkdir, readdir, readFile, stat } from 'node:fs/promises';
import { homedir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

/** Directory of the bundled skill (skills/jsui in this package). */
export const SKILL_DIR = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'skills', 'jsui');

export const SKILL_USAGE = `  jsui skill [print] [--all]            print the agent skill (SKILL.md; --all: with its references)
  jsui skill install [dir] [--global] [--force]
                                        copy it to <dir>/.claude/skills/jsui (default: .), or
                                        ~/.claude/skills/jsui with --global
  jsui skill path                       where the skill's files are`;

async function exists(p) {
  try {
    await stat(p);
    return true;
  } catch {
    return false;
  }
}

export async function skillCommand(args) {
  const { values, positionals } = parseArgs({
    args,
    allowPositionals: true,
    options: { all: { type: 'boolean' }, global: { type: 'boolean' }, force: { type: 'boolean' }, help: { type: 'boolean', short: 'h' } },
  });
  const sub = positionals[0] || 'print';
  if (values.help) {
    console.log('usage:\n' + SKILL_USAGE);
    return 0;
  }
  if (sub === 'path') {
    console.log(SKILL_DIR);
    return 0;
  }
  if (sub === 'print') {
    let out = await readFile(path.join(SKILL_DIR, 'SKILL.md'), 'utf8');
    if (values.all) {
      const refs = (await readdir(path.join(SKILL_DIR, 'references'))).filter((f) => f.endsWith('.md')).sort();
      for (const f of refs) out += `\n\n<!-- references/${f} -->\n\n` + (await readFile(path.join(SKILL_DIR, 'references', f), 'utf8'));
    }
    process.stdout.write(out.endsWith('\n') ? out : out + '\n');
    return 0;
  }
  if (sub === 'install') {
    const base = values.global ? homedir() : path.resolve(positionals[1] || '.');
    const dest = path.join(base, '.claude', 'skills', 'jsui');
    if ((await exists(dest)) && !values.force) {
      const old = await readFile(path.join(dest, 'SKILL.md'), 'utf8').catch(() => '');
      const cur = await readFile(path.join(SKILL_DIR, 'SKILL.md'), 'utf8');
      if (old !== cur) {
        console.error(`jsui: ${dest} exists and differs; pass --force to replace it`);
        return 1;
      }
    }
    await mkdir(path.dirname(dest), { recursive: true });
    await cp(SKILL_DIR, dest, { recursive: true, force: true });
    console.log(`jsui: installed the JSUI skill in ${dest}`);
    return 0;
  }
  console.error(`jsui skill: unknown subcommand '${sub}'\nusage:\n` + SKILL_USAGE);
  return 2;
}
