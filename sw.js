
const CACHE = "arcade-room-1f024537c90d39e4";
const FILES = ["index.html","icon.svg","manifest.webmanifest","LICENSE.txt","THIRD_PARTY_NOTICES.txt","assets/index-BKd0NNq1.css","assets/index--S9QYxmg.js"];
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
