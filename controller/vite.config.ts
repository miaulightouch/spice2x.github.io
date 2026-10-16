import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { defineConfig, type Plugin } from 'vite';
import vue from '@vitejs/plugin-vue';

const precachePublicFiles = ['icon.svg', 'manifest.webmanifest', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt'];

export function offlinePlugin(): Plugin {
  let publicDir: string | false = false;
  return {
    name: 'arcade-room-offline',
    apply: 'build',
    enforce: 'post',
    configResolved(config) { publicDir = config.publicDir; },
    generateBundle(_, bundle) {
      const files = [...new Set([
        'index.html', ...precachePublicFiles, ...Object.keys(bundle).filter(k => !k.endsWith('.map')),
      ])].sort();
      const hash = createHash('sha256');
      for (const fileName of files) {
        hash.update(fileName).update('\0');
        const output = bundle[fileName];
        if (output) hash.update(output.type === 'asset' ? output.source : output.code);
        else if (publicDir) hash.update(readFileSync(join(publicDir, fileName)));
        hash.update('\0');
      }
      const cache = 'arcade-room-' + hash.digest('hex').slice(0, 16);
      this.emitFile({ type: 'asset', fileName: 'sw.js', source: `
const CACHE = ${JSON.stringify(cache)};
const FILES = ${JSON.stringify(files)};
const ROOT = new URL('./', self.location.href);
const URLS = FILES.map(path => new URL(path, ROOT).href);
self.addEventListener('install', event => event.waitUntil(caches.open(CACHE).then(cache => cache.addAll(URLS))));
self.addEventListener('activate', event => event.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(key => key.startsWith('arcade-room-') && key !== CACHE).map(key => caches.delete(key)))).then(() => self.clients.claim())));
self.addEventListener('fetch', event => {
  const request = event.request;
  if (request.method !== 'GET' || new URL(request.url).origin !== ROOT.origin) return;
  if (request.mode === 'navigate' && new URL(request.url).pathname.startsWith(ROOT.pathname)) {
    event.respondWith(fetch(request).catch(() => caches.open(CACHE).then(cache => cache.match(new URL('index.html', ROOT).href))));
  } else if (URLS.includes(request.url)) {
    event.respondWith(caches.open(CACHE).then(async cache => (await cache.match(request)) || fetch(request)));
  }
});
` });
    },
  };
}

export default defineConfig({
  base: './',
  plugins: [vue(), offlinePlugin()],
});
