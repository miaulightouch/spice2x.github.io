import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, mkdtempSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const git = (...args) => execFileSync('git', ['-C', root, ...args], { stdio: 'inherit' });
const npmCli = process.env.npm_execpath;
if (!npmCli) throw new Error('Run this script with npm run deploy:pages.');
execFileSync(process.execPath, [npmCli, 'run', 'build'], { cwd: root, stdio: 'inherit' });
const dist = resolve(root, 'dist');
if (!existsSync(join(dist, 'index.html'))) throw new Error('Missing built index.html');
git('fetch', 'origin', 'refs/heads/gh-pages');
const directory = mkdtempSync(join(tmpdir(), 'arcade-room-pages-'));
const checkout = join(directory, 'checkout');
let attached = false;
try {
  git('worktree', 'add', '--detach', checkout, 'FETCH_HEAD'); attached = true;
  for (const entry of readdirSync(checkout)) {
    if (entry !== '.git') rmSync(join(checkout, entry), { recursive: true, force: true });
  }
  cpSync(dist, checkout, { recursive: true });
  const command = (...args) => execFileSync('git', ['-C', checkout, ...args], { stdio: 'inherit' });
  command('add', '--all');
  const changed = execFileSync('git', ['-C', checkout, 'diff', '--cached', '--name-only'], { encoding: 'utf8' }).trim();
  if (changed) {
    command('commit', '-m', 'Publish Arcade Room controller');
    command('push', 'origin', 'HEAD:refs/heads/gh-pages');
  } else console.log('GitHub Pages already has this build.');
} finally {
  if (attached) git('worktree', 'remove', '--force', checkout);
  rmSync(directory, { recursive: true, force: true });
}
