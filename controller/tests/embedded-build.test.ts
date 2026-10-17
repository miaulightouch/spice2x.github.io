import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { build } from 'vite';
import { embeddedPlugin, offlinePlugin } from '../vite.config';

async function writeFixture(root: string) {
  await mkdir(join(root, 'src'), { recursive: true });
  await mkdir(join(root, 'public'), { recursive: true });
  await Promise.all([
    writeFile(join(root, 'index.html'), '<!doctype html><html><head><link rel="icon" href="./icon.svg"><link rel="manifest" href="./manifest.webmanifest"></head><body><div id="app"></div><script type="module" src="/src/main.ts"></script></body></html>'),
    writeFile(join(root, 'src/main.ts'), `import './style.css'; document.body.dataset.marker = '</script>';`),
    writeFile(join(root, 'src/style.css'), `body::after { content: '</style>'; }`),
    writeFile(join(root, 'public/icon.svg'), '<svg xmlns="http://www.w3.org/2000/svg"><path d="one"/></svg>'),
    writeFile(join(root, 'public/manifest.webmanifest'), '{"name":"Remote"}'),
    writeFile(join(root, 'public/LICENSE.txt'), 'license notice'),
    writeFile(join(root, 'public/THIRD_PARTY_NOTICES.txt'), 'third-party notice'),
  ]);
}

async function buildFixture(root: string, mode: string) {
  await build({
    root,
    mode,
    base: './',
    configFile: false,
    logLevel: 'silent',
    plugins: [offlinePlugin(), embeddedPlugin()],
  });
}

test('embedded build is a single self-contained HTML document', async () => {
  const root = await mkdtemp(join(tmpdir(), 'arcade-room-embedded-'));
  try {
    await writeFixture(root);
    await buildFixture(root, 'embedded');
    const output = join(root, 'dist', 'embedded');
    assert.deepEqual(await readdir(output), ['index.html']);
    const html = await readFile(join(output, 'index.html'), 'utf8');
    assert.match(html, /<script type="module"[^>]*>/);
    assert.match(html, /<style>/);
    assert.match(html, /data:image\/svg\+xml,/);
    assert.match(html, /license notice/);
    assert.match(html, /third-party notice/);
    assert.doesNotMatch(html, /<script[^>]+src=/i);
    assert.doesNotMatch(html, /<link[^>]+(?:stylesheet|manifest)[^>]*>/i);
    assert.doesNotMatch(html, /(?:assets\/|sw\.js|manifest\.webmanifest|import\s*\()/);
    const script = html.match(/<script type="module">([\s\S]*)<\/script>/i)?.[1] ?? '';
    const style = html.match(/<style>([\s\S]*)<\/style>/i)?.[1] ?? '';
    assert.doesNotMatch(script, /<\/script/i);
    assert.doesNotMatch(style, /<\/style/i);
  } finally { await rm(root, { recursive: true, force: true }); }
});

test('Pages build still emits the offline cache and public assets', async () => {
  const root = await mkdtemp(join(tmpdir(), 'arcade-room-pages-'));
  try {
    await writeFixture(root);
    await buildFixture(root, 'production');
    const output = join(root, 'dist');
    const files = await readdir(output);
    assert.ok(files.includes('sw.js'));
    assert.ok(files.includes('icon.svg'));
    assert.ok(files.includes('manifest.webmanifest'));
    assert.ok(files.includes('assets'));
    const worker = await readFile(join(output, 'sw.js'), 'utf8');
    assert.match(worker, /\.js/);
    assert.match(worker, /\.css/);
  } finally { await rm(root, { recursive: true, force: true }); }
});
