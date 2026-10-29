import assert from 'node:assert/strict';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { createSSRApp } from 'vue';
import { renderToString } from 'vue/server-renderer';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';

test('player exposes play, stop and fullscreen, including cancellation during startup', async () => {
  const server = await createServer({
    root: fileURLToPath(new URL('..', import.meta.url)), configFile: false,
    plugins: [vue()], server: { middlewareMode: true, hmr: false }, appType: 'custom',
  });
  try {
    const { default: Player } = await server.ssrLoadModule('/src/components/CapturePlayer.vue');
    const states = [
      { streamUrl: '', streamPending: false, playDisabled: false, playEnabled: true, stopEnabled: false },
      { streamUrl: '', streamPending: true, playDisabled: true, playEnabled: false, stopEnabled: true },
      { streamUrl: 'http://127.0.0.1/stream.mjpg', streamPending: false, playDisabled: false, playEnabled: false, stopEnabled: true },
      { streamUrl: '', streamPending: false, playDisabled: true, playEnabled: false, stopEnabled: false },
    ];
    for (const state of states) {
      const { playEnabled, stopEnabled, ...props } = state;
      const html = await renderToString(createSSRApp(Player, { ...props, image: '', active: true }));
      const buttons = [...html.matchAll(/<button\b([^>]*)>([\s\S]*?)<\/button>/g)];
      assert.equal(buttons.length, 3);
      assert.equal(!/\bdisabled\b/.test(buttons[0]![1]!), playEnabled);
      assert.equal(!/\bdisabled\b/.test(buttons[1]![1]!), stopEnabled);
      assert.doesNotMatch(buttons[2]![1]!, /\bdisabled\b/);
      for (const button of buttons) assert.ok(button[2]!.replace(/<[^>]*>/g, '').trim().length > 1);
      assert.equal(html.includes('<img'), !!state.streamUrl);
    }
  } finally { await server.close(); }
});
