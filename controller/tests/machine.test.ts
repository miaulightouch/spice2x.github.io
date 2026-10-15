import assert from 'node:assert/strict';
import { once } from 'node:events';
import { test } from 'node:test';
import type { AddressInfo } from 'node:net';
import { WebSocketServer } from 'ws';
import { Rc4 } from '../src/api/rc4';
import { DemoApi } from '../src/api/demo';
import { useController } from '../src/useController';

async function machine(mode: 'ok' | 'reject' | 'close' | 'resize-error', password = 'secret') {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await once(server, 'listening');
  const seen: string[] = [];
  const api = new DemoApi();
  const encoder = new TextEncoder();
  server.on('connection', socket => {
    let cipher = password ? new Rc4(encoder.encode(password)) : undefined;
    socket.on('message', async raw => {
      const bytes = new Uint8Array(raw as Buffer);
      const decoded = cipher ? cipher.crypt(bytes) : bytes;
      const request = JSON.parse(new TextDecoder().decode(decoded.subarray(0, -1)));
      seen.push(request.function);
      const refresh = request.function === 'session_refresh';
      const system = request.module === 'control' && !refresh;
      if (system && mode === 'close') { socket.close(); return; }
      const errors = system && mode === 'reject' ? ['Unable to acquire shutdown privileges'] :
        mode === 'resize-error' && request.function === 'image_resize_set' ? ['resize rejected'] : [];
      const data = errors.length ? [] : refresh ? ['session-key'] : await api.request(request.module, request.function, request.params);
      const out = encoder.encode(JSON.stringify({ id: request.id, errors, data }) + '\0');
      socket.send(cipher ? cipher.crypt(out) : out);
      if (refresh) cipher = new Rc4(encoder.encode('session-key'));
    });
  });
  return {
    seen, profile: { id: 'test', name: 'Test', endpoint: `ws://127.0.0.1:${(server.address() as AddressInfo).port}`, password, rememberPassword: false },
    async close() { for (const socket of server.clients) socket.terminate(); server.close(); await once(server, 'close'); },
  };
}

for (const command of ['restart', 'exit', 'reboot', 'shutdown'] as const) {
  test(`system command ${command} is sent once after pending calibration and disconnects`, async () => {
    const m = await machine('ok'); const c = useController();
    try {
      await c.connect(m.profile); c.updateResize({ offset_x: 20 });
      await c.machineControl(command);
      assert.equal(c.error.value, ''); assert.equal(c.connected.value, false);
      assert.equal(m.seen.filter(fn => fn === command).length, 1);
      assert.ok(m.seen.indexOf('image_resize_set') < m.seen.indexOf(command));
    } finally { c.disconnect(); await m.close(); }
  });
}

test('system API error preserves the connection and reports the error', async () => {
  const m = await machine('reject'); const c = useController();
  try {
    await c.connect(m.profile); await c.machineControl('shutdown');
    assert.match(c.error.value, /privileges/); assert.equal(c.connected.value, true);
  } finally { c.disconnect(); await m.close(); }
});

test('termination before response reports uncertain result without replaying', async () => {
  const m = await machine('close'); const c = useController();
  try {
    await c.connect(m.profile); await c.machineControl('restart');
    assert.match(c.message.value, /無法確認執行結果/);
    assert.equal(c.connected.value, false); assert.equal(m.seen.filter(fn => fn === 'restart').length, 1);
  } finally { c.disconnect(); await m.close(); }
});

test('system action requires the configured API password', async () => {
  const m = await machine('ok', ''); const c = useController();
  try {
    await c.connect(m.profile); await c.machineControl('shutdown');
    assert.match(c.error.value, /API 密碼/); assert.ok(!m.seen.includes('shutdown'));
  } finally { c.disconnect(); await m.close(); }
});

test('failed background calibration cannot be reported as successfully saved', async () => {
  const m = await machine('resize-error'); const c = useController();
  try {
    await c.connect(m.profile); c.updateResize({ offset_x: 10 });
    for (let i = 0; i < 100 && !c.error.value; i++) await new Promise(resolve => setTimeout(resolve, 10));
    assert.match(c.error.value, /resize rejected/);
    await c.saveResize();
    assert.match(c.error.value, /校正未成功套用/); assert.ok(!m.seen.includes('image_resize_save'));
  } finally { c.disconnect(); await m.close(); }
});
