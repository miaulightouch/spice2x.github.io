import { createHash } from 'node:crypto';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  base: './',
  plugins: [vue(), {
    name: 'arcade-room-offline',
    apply: 'build',
    generateBundle(_, bundle) {
      const files = ['index.html', 'icon.svg', 'manifest.webmanifest', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', ...Object.keys(bundle).filter(k => !k.endsWith('.map'))];
      const cache = 'arcade-room-' + createHash('sha256').update(JSON.stringify(bundle)).digest('hex').slice(0, 16);
      this.emitFile({ type: 'asset', fileName: 'sw.js', source: `
const CACHE = ${JSON.stringify(cache)};
const FILES = ${JSON.stringify([...new Set(files)])};
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
  }],
});
