import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { build } from 'vite';
import { offlinePlugin } from '../vite.config';

async function writeFixture(root: string, title = 'Arcade Room', icon = '<svg>one</svg>') {
  await mkdir(join(root, 'src'), { recursive: true });
  await mkdir(join(root, 'public'), { recursive: true });
  await Promise.all([
    writeFile(join(root, 'index.html'), `<!doctype html><html><head><title>${title}</title><link rel="icon" href="./icon.svg"><link rel="manifest" href="./manifest.webmanifest"><link rel="stylesheet" href="/src/style.css"></head><body><script type="module" src="/src/main.ts"></script></body></html>`),
    writeFile(join(root, 'src/main.ts'), `import './style.css'; document.body.dataset.ready = 'yes';`),
    writeFile(join(root, 'src/style.css'), 'body { color: teal; }'),
    writeFile(join(root, 'public/icon.svg'), icon),
    writeFile(join(root, 'public/manifest.webmanifest'), '{"name":"Arcade Room"}'),
    writeFile(join(root, 'public/LICENSE.txt'), 'license'),
    writeFile(join(root, 'public/THIRD_PARTY_NOTICES.txt'), 'notice'),
  ]);
}

async function buildFixture(root: string) {
  await build({ root, base: './', configFile: false, logLevel: 'silent', plugins: [offlinePlugin()] });
  return readFile(join(root, 'dist/sw.js'), 'utf8');
}

function readServiceWorker(source: string) {
  const cache = source.match(/const CACHE = "([^"]+)";/)?.[1];
  const files = source.match(/const FILES = (\[[^\n]+\]);/)?.[1];
  assert.ok(cache); assert.ok(files);
  return { cache, files: JSON.parse(files) as string[] };
}

test('public-only and final HTML changes invalidate the offline cache', async () => {
  const root = await mkdtemp(join(tmpdir(), 'arcade-room-offline-'));
  try {
    await writeFixture(root);
    const first = readServiceWorker(await buildFixture(root));
    await writeFile(join(root, 'public/icon.svg'), '<svg>two</svg>');
    const publicChange = readServiceWorker(await buildFixture(root));
    assert.notEqual(publicChange.cache, first.cache);
    assert.deepEqual(publicChange.files, first.files);
    assert.ok(first.files.includes('index.html'));
    assert.ok(first.files.includes('icon.svg'));
    assert.ok(first.files.includes('manifest.webmanifest'));
    assert.ok(first.files.includes('LICENSE.txt'));
    assert.ok(first.files.includes('THIRD_PARTY_NOTICES.txt'));
    assert.ok(first.files.some(file => file.endsWith('.js')));
    assert.ok(first.files.some(file => file.endsWith('.css')));

    await writeFile(join(root, 'index.html'), (await readFile(join(root, 'index.html'), 'utf8')).replace('Arcade Room', 'Changed title'));
    const htmlChange = readServiceWorker(await buildFixture(root));
    assert.notEqual(htmlChange.cache, publicChange.cache);
  } finally { await rm(root, { recursive: true, force: true }); }
});

test('identical source in different checkouts gets the same offline cache', async () => {
  const roots = await Promise.all([mkdtemp(join(tmpdir(), 'arcade-room-offline-')), mkdtemp(join(tmpdir(), 'arcade-room-offline-'))]);
  try {
    await Promise.all(roots.map(root => writeFixture(root)));
    const workers = await Promise.all(roots.map(buildFixture));
    assert.equal(readServiceWorker(workers[0]).cache, readServiceWorker(workers[1]).cache);
    assert.equal(workers[0], workers[1]);
  } finally { await Promise.all(roots.map(root => rm(root, { recursive: true, force: true }))); }
});
