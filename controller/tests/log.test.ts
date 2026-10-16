import assert from 'node:assert/strict';
import { once } from 'node:events';
import type { AddressInfo } from 'node:net';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { LocalizedError } from '../src/i18n';
import { downloadLog, readLog } from '../src/api/log';
import type { Rpc } from '../src/api/client';
import { Rc4 } from '../src/api/rc4';
import { useController } from '../src/useController';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

class RpcFake implements Rpc {
  readonly requests: { module: string; fn: string; params: unknown[] }[] = [];
  constructor(private readonly responses: unknown[][]) {}
  async request(module: string, fn: string, params: unknown[] = []): Promise<unknown[]> {
    this.requests.push({ module, fn, params });
    const response = this.responses.shift();
    if (!response) throw new Error('missing fake response');
    return response;
  }
}

function logResponse(offset: number, bytes: Uint8Array, size = offset + bytes.length): unknown[] {
  return [{ offset, next: offset + bytes.length, size, data: Buffer.from(bytes).toString('base64') }];
}

test('readLog accepts binary base64 and preserves a tail response offset', async () => {
  const bytes = Uint8Array.from([0, 255, 10, 128]);
  const rpc = new RpcFake([logResponse(17, bytes, 21)]);
  const result = await readLog(rpc, null, 64);
  assert.deepEqual([...result.bytes], [...bytes]);
  assert.deepEqual(rpc.requests, [{ module: 'info', fn: 'log', params: [null, 64] }]);
  assert.equal(result.offset, 17);
  assert.equal(result.next, 21);
  assert.equal(result.size, 21);
});

test('downloadLog joins UTF-8 bytes split across chunks', async () => {
  const full = encoder.encode('prefix-中文-尾\n');
  const split = 8; // Split inside the first CJK code point.
  const rpc = new RpcFake([
    logResponse(0, full.slice(0, split), full.length),
    logResponse(split, full.slice(split), full.length),
  ]);
  const file = await downloadLog(rpc, () => true);
  const bytes = new Uint8Array(await file.arrayBuffer());
  assert.deepEqual([...bytes], [...full]);
  assert.equal(new TextDecoder().decode(bytes), 'prefix-中文-尾\n');
});

test('downloadLog takes the initial size snapshot when the log grows', async () => {
  const rpc = new RpcFake([
    logResponse(0, Uint8Array.from([1, 2, 3]), 6),
    logResponse(3, Uint8Array.from([4, 5, 6]), 99),
  ]);
  const file = await downloadLog(rpc, () => true);
  assert.deepEqual([...new Uint8Array(await file.arrayBuffer())], [1, 2, 3, 4, 5, 6]);
  assert.equal(rpc.requests.length, 2);
});

test('log downloads reject malformed, truncated, zero-progress and cancelled responses', async () => {
  const invalid = new RpcFake([[{ offset: 0, next: 1, size: 1, data: '@@@' }]]);
  await assert.rejects(readLog(invalid), error => error instanceof LocalizedError && error.key === 'error.logFormat');

  const truncated = new RpcFake([logResponse(0, Uint8Array.from([1]), 2), logResponse(1, new Uint8Array(), 1)]);
  await assert.rejects(downloadLog(truncated, () => true), error => error instanceof LocalizedError && error.key === 'error.logChanged');

  const zero = new RpcFake([logResponse(0, new Uint8Array(), 2)]);
  await assert.rejects(downloadLog(zero, () => true), error => error instanceof LocalizedError && error.key === 'error.logChanged');

  const cancelled = new RpcFake([logResponse(0, Uint8Array.from([1]), 2)]);
  let checks = 0;
  await assert.rejects(downloadLog(cancelled, () => checks++ === 0), error => error instanceof LocalizedError && error.key === 'error.cancelled');
  assert.equal(cancelled.requests.length, 1);
});

interface Request { id: number; module: string; function: string; params: unknown[] }
interface LogServerOptions { password?: string; delayFirstLog?: boolean }

async function logServer(options: LogServerOptions = {}) {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await once(server, 'listening');
  const address = server.address() as AddressInfo;
  const requests: Request[] = [];
  let connectionNumber = 0;
  let firstLog!: () => void;
  const firstLogSeen = new Promise<void>(resolve => { firstLog = resolve; });

  server.on('connection', socket => {
    const connection = ++connectionNumber;
    let cipher = options.password ? new Rc4(encoder.encode(options.password)) : undefined;
    const send = (request: Request, data: unknown[], errors: string[] = []): void => {
      const bytes = encoder.encode(JSON.stringify({ id: request.id, errors, data }) + '\0');
      socket.send(cipher ? cipher.crypt(bytes) : bytes);
    };
    socket.on('message', (raw, binary) => {
      assert.equal(binary, true);
      const bytes = cipher ? cipher.crypt(new Uint8Array(raw as Buffer)) : new Uint8Array(raw as Buffer);
      assert.equal(bytes.at(-1), 0);
      const request = JSON.parse(decoder.decode(bytes.subarray(0, -1))) as Request;
      requests.push(request);
      if (request.function === 'session_refresh') {
        const key = `log-session-${connection}`;
        send(request, [key]);
        cipher = new Rc4(encoder.encode(key));
        return;
      }
      if (request.module === 'info' && request.function === 'log') {
        if (connection === 1) firstLog();
        const text = encoder.encode(connection === 1 ? 'old log\n' : 'new log\n');
        const reply = (): void => { if (socket.readyState === 1) send(request, [{ offset: 0, next: text.length, size: text.length, data: Buffer.from(text).toString('base64') }]); };
        if (options.delayFirstLog && connection === 1) setTimeout(reply, 80); else reply();
        return;
      }
      if (request.module === 'buttons' && request.function === 'read') { send(request, [['Service', 0, false]]); return; }
      if (request.module === 'info' && request.function === 'avs') { send(request, [{ model: 'TEST' }]); return; }
      if (request.module === 'info' && request.function === 'launcher') { send(request, [{ version: 'test' }]); return; }
      if (request.module === 'resize' && request.function === 'image_resize_get') {
        const scene = { offset_x: 0, offset_y: 0, scale_x: 1, scale_y: 1, keep_aspect_ratio: false, duplicate: 0 };
        send(request, [{ enabled: true, scene: 1, linear_filter: true, scenes: [scene, scene, scene, scene] }]);
        return;
      }
      if (request.module === 'coin' && request.function === 'get') { send(request, [0]); return; }
      send(request, []);
    });
  });
  return {
    endpoint: `ws://127.0.0.1:${address.port}`,
    requests,
    firstLogSeen,
    async close(): Promise<void> {
      for (const client of server.clients) client.terminate();
      await new Promise<void>(resolve => server.close(() => resolve()));
    },
  };
}

test('readLog requires a password and demo log access never opens a network connection', async () => {
  const server = await logServer();
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password: '', rememberPassword: false });
    await controller.readLog();
    assert.notEqual(controller.error.value, '');
    assert.equal(server.requests.some(request => request.module === 'info' && request.function === 'log'), false);
  } finally {
    controller.disconnect();
    await server.close();
  }

  const demo = useController();
  try {
    await demo.startDemo();
    await demo.readLog();
    const file = await demo.downloadLog();
    assert.match(demo.logText.value, /DEMO: offline log viewer/);
    assert.ok(file);
    assert.match(await file!.text(), /DEMO: no cabinet connection/);
  } finally { demo.disconnect(); }
});

test('a delayed log response from a disconnected session cannot overwrite a new session', async () => {
  const server = await logServer({ password: 'secret', delayFirstLog: true });
  const controller = useController();
  try {
    const profile = { id: 'test', name: 'test', endpoint: server.endpoint, password: 'secret', rememberPassword: false };
    await controller.connect(profile);
    const oldRead = controller.readLog();
    await server.firstLogSeen;
    controller.disconnect();
    await oldRead;
    await controller.connect(profile);
    await controller.readLog();
    assert.equal(controller.logText.value, 'new log\n');
    await new Promise(resolve => setTimeout(resolve, 100));
    assert.equal(controller.logText.value, 'new log\n');
  } finally {
    controller.disconnect();
    await server.close();
  }
});
