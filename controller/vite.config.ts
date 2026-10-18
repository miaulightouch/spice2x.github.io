import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig, type Plugin } from 'vite';
import vue from '@vitejs/plugin-vue';

const precachePublicFiles = ['icon.svg', 'manifest.webmanifest', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt'];
const buildDate = new Date().toISOString().slice(2, 10);
const buildCommit = execFileSync('git', ['rev-parse', '--short=7', 'HEAD'], {
  cwd: fileURLToPath(new URL('..', import.meta.url)), encoding: 'utf8',
}).trim();

export function offlinePlugin(): Plugin {
  let publicDir: string | false = false;
  let embedded = false;
  return {
    name: 'arcade-room-offline',
    apply: 'build',
    enforce: 'post',
    configResolved(config) { publicDir = config.publicDir; embedded = config.mode === 'embedded'; },
    generateBundle(_, bundle) {
      if (embedded) return;
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

function escapeHtml(source: string) {
  return source.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
}

function inlineReference(bundle: Record<string, { type: string; code?: string; source?: string | Uint8Array }>, reference: string) {
  const fileName = decodeURIComponent(reference).replace(/^\.\//, '').replace(/^\//, '');
  const output = bundle[fileName];
  if (!output) throw new Error(`embedded build is missing ${fileName}`);
  return output.type === 'chunk' ? output.code ?? '' : String(output.source ?? '');
}

export function embeddedPlugin(): Plugin {
  let embedded = false;
  let publicDir = '';
  return {
    name: 'arcade-room-embedded',
    apply: 'build',
    enforce: 'post',
    config(_, env) {
      if (env.mode !== 'embedded') return;
      return {
        build: {
          outDir: 'dist/embedded',
          emptyOutDir: true,
          assetsInlineLimit: Number.MAX_SAFE_INTEGER,
          cssCodeSplit: false,
          rollupOptions: { output: { inlineDynamicImports: true } },
        },
        publicDir: false,
      };
    },
    configResolved(config) {
      embedded = config.mode === 'embedded';
      publicDir = join(config.root, 'public');
    },
    generateBundle(_, bundle) {
      if (!embedded) return;
      const html = bundle['index.html'];
      if (!html || html.type !== 'asset') throw new Error('embedded build did not produce index.html');
      let source = String(html.source);
      source = source.replace(/<script\b([^>]*?)\bsrc=(['"])([^'"]+)\2([^>]*)><\/script>/gi,
        (tag, before: string, _quote: string, reference: string, after: string) => {
          if (!/\btype=(['"])module\1/i.test(`${before}${after}`)) return tag;
          const code = inlineReference(bundle, reference).replace(/<\/script/gi, '<\\/script');
          return `<script${before}${after}>${code}</script>`;
        });
      source = source.replace(/<link\b(?=[^>]*\brel=(['"])[^'"]*stylesheet[^'"]*\1)[^>]*\bhref=(['"])([^'"]+)\2[^>]*>/gi,
        (_tag, _relQuote: string, _hrefQuote: string, reference: string) => {
          const css = inlineReference(bundle, reference).replace(/<\/style/gi, '<\\/style');
          return `<style>${css}</style>`;
        });
      source = source.replace(/<link\b(?=[^>]*\brel=(['"])manifest\1)[^>]*>/gi, '');
      const icon = join(publicDir, 'icon.svg');
      if (!existsSync(icon)) throw new Error('embedded build requires public/icon.svg');
      const iconData = `data:image/svg+xml,${encodeURIComponent(readFileSync(icon, 'utf8'))}`;
      source = source.replace(/<link\b(?=[^>]*\brel=(['"])icon\1)[^>]*>/i,
        `<link rel="icon" type="image/svg+xml" href="${iconData}">`);
      const notices = ['LICENSE.txt', 'THIRD_PARTY_NOTICES.txt']
        .filter(file => existsSync(join(publicDir, file)))
        .map(file => `<template data-embedded-notice="${file}">${escapeHtml(readFileSync(join(publicDir, file), 'utf8'))}</template>`)
        .join('');
      source = source.replace(/<\/body>/i, `${notices}</body>`);
      if (/\b(?:src|href)=(['"])(?!data:|#)[^'"]+\1/i.test(source)) {
        throw new Error('embedded build still contains an external asset reference');
      }
      html.source = source;
      for (const fileName of Object.keys(bundle)) if (fileName !== 'index.html') delete bundle[fileName];
    },
  };
}

export default defineConfig({
  base: './',
  define: {
    'import.meta.env.VITE_BUILD_VERSION': JSON.stringify(`${buildDate}-${buildCommit}`),
  },
  plugins: [vue(), offlinePlugin(), embeddedPlugin()],
});
