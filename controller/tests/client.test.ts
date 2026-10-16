import assert from 'node:assert/strict';
import { once } from 'node:events';
import { test } from 'node:test';
import type { AddressInfo } from 'node:net';
import { WebSocketServer } from 'ws';
import { ApiError, SpiceClient, normalizeEndpoint } from '../src/api/client';
import { Rc4 } from '../src/api/rc4';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
interface Request { id: number; module: string; function: string; params: unknown[] }
interface MockOptions { password?: string; fail?: boolean; wrongId?: boolean; hang?: boolean; split?: boolean }
async function mock(options: MockOptions = {}) {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await once(server, 'listening');
  const seen: Request[] = [];
  const violations: string[] = [];
  server.on('connection', socket => {
    let cipher = options.password ? new Rc4(encoder.encode(options.password)) : undefined;
    let inFlight = false;
    socket.on('message', (raw, binary) => {
      if (!binary) violations.push('not binary');
      if (inFlight) violations.push('request pipelined before response');
      const bytes = cipher ? cipher.crypt(new Uint8Array(raw as Buffer)) : new Uint8Array(raw as Buffer);
      let req: Request;
      try { assert.equal(bytes.at(-1), 0); req = JSON.parse(decoder.decode(bytes.subarray(0, -1))); }
      catch { violations.push('decrypt/UTF-8/NUL failure'); socket.close(); return; }
      seen.push(req);
      inFlight = true;
      const refresh = req.function === 'session_refresh';
      if (options.hang && !refresh) return;
      setTimeout(() => {
        const data = refresh ? ['new-session-金鑰'] : req.params;
        const out = encoder.encode(JSON.stringify({ id: options.wrongId && !refresh ? req.id + 1 : req.id,
          errors: options.fail && !refresh ? ['Example API failure'] : [], data }) + '\0');
        const encrypted = cipher ? cipher.crypt(out) : out;
        if (options.split && !refresh) {
          socket.send(encrypted.subarray(0, 4));
          socket.send(encrypted.subarray(4));
        } else socket.send(encrypted);
        if (refresh) cipher = new Rc4(encoder.encode(String(data[0])));
        inFlight = false;
      }, 5);
    });
  });
  return { endpoint: `ws://127.0.0.1:${(server.address() as AddressInfo).port}`, seen, violations,
    async close() { for (const client of server.clients) client.terminate(); server.close(); await once(server, 'close'); } };
}

test('RC4 matches a published known vector and preserves stream position between chunks', () => {
  const cipher = new Rc4(encoder.encode('Key'));
  const a = cipher.crypt(encoder.encode('Plain'));
  const b = cipher.crypt(encoder.encode('text'));
  assert.equal(Buffer.concat([a, b]).toString('hex'), 'bbf316e8d940af0ad3');
});

for (const password of ['', '密碼-with-UTF8']) {
  test(`binary NUL framing, shared bidirectional cipher and rekey (${password ? 'password' : 'no password'})`, async () => {
    const s = await mock({ password }); const c = new SpiceClient();
    try {
      await c.connect(s.endpoint, password);
      const result = await Promise.all(Array.from({ length: 12 }, (_, i) => c.request('echo', 'value', [i, '中文'])));
      assert.deepEqual(result, Array.from({ length: 12 }, (_, i) => [i, '中文']));
      await c.request('control', 'session_refresh');
      assert.deepEqual(await c.request('echo', 'value', ['after rekey']), ['after rekey']);
      assert.equal(s.seen[0]!.function, 'session_refresh');
      assert.deepEqual(s.violations, []);
    } finally { c.disconnect(); await s.close(); }
  });
}

test('queued request payloads are snapshots and fragmented binary responses are accepted', async () => {
  const s = await mock({ split: true }); const c = new SpiceClient();
  try {
    await c.connect(s.endpoint, '');
    const first = c.request('echo', 'value', [0]);
    const patch = { scale_x: 1.2 };
    const second = c.request('echo', 'value', [patch]); patch.scale_x = 2;
    await first; assert.deepEqual(await second, [{ scale_x: 1.2 }]);
    assert.deepEqual(s.violations, []);
  } finally { c.disconnect(); await s.close(); }
});

test('API errors preserve a synchronized connection for later calls', async () => {
  const s = await mock({ fail: true }); const c = new SpiceClient();
  try {
    await c.connect(s.endpoint, '');
    await assert.rejects(c.request('echo', 'value'), ApiError);
    await assert.rejects(c.request('echo', 'value'), ApiError);
    assert.equal(c.status, 'connected'); assert.deepEqual(s.violations, []);
  } finally { c.disconnect(); await s.close(); }
});

test('timeout closes the connection and queued writes are never replayed', async () => {
  const s = await mock({ hang: true }); const c = new SpiceClient(50);
  try {
    await c.connect(s.endpoint, '');
    const results = await Promise.allSettled([c.request('echo', 'hang'), c.request('coin', 'insert', [1])]);
    assert.ok(results.every(r => r.status === 'rejected'));
    assert.equal(c.status, 'error');
    assert.equal(s.seen.filter(r => r.module === 'coin').length, 0);
  } finally { c.disconnect(); await s.close(); }
});

test('a response with a wrong request ID fails closed', async () => {
  const s = await mock({ wrongId: true }); const c = new SpiceClient();
  try {
    await c.connect(s.endpoint, '');
    await assert.rejects(c.request('echo', 'value'), /編號/);
    assert.equal(c.status, 'error');
  } finally { c.disconnect(); await s.close(); }
});

test('wrong initial password cannot produce a connected session', async () => {
  const s = await mock({ password: 'correct' }); const c = new SpiceClient(80);
  try {
    await assert.rejects(c.connect(s.endpoint, 'wrong'));
    assert.equal(c.status, 'error');
  } finally { c.disconnect(); await s.close(); }
});

test('URL validation rejects embedded credentials and non-WebSocket schemes', () => {
  assert.equal(normalizeEndpoint(' ws://192.168.1.10:1338 '), 'ws://192.168.1.10:1338/');
  assert.equal(normalizeEndpoint('wss://example.com/spice'), 'wss://example.com/spice');
  for (const value of ['https://example.com', 'ws://user:pass@example.com', 'ws://example.com/#password']) assert.throws(() => normalizeEndpoint(value));
});
