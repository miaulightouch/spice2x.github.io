import assert from 'node:assert/strict';
import { once } from 'node:events';
import type { AddressInfo } from 'node:net';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { LocalizedError } from '../src/i18n';
import { parseCaptureFrame, parseCaptureScreens, readCaptureFrame, readCaptureScreens } from '../src/api/capture';
import type { Rpc } from '../src/api/client';
import { Rc4 } from '../src/api/rc4';
import { useController } from '../src/useController';

class CaptureRpc implements Rpc {
  readonly requests: { module: string; fn: string; params: unknown[] }[] = [];
  constructor(private readonly responses: unknown[][]) {}
  async request(module: string, fn: string, params: unknown[] = []): Promise<unknown[]> {
    this.requests.push({ module, fn, params });
    const response = this.responses.shift();
    if (!response) throw new Error('missing fake response');
    return response;
  }
}

test('capture parsers deduplicate valid screens and preserve frame metadata', () => {
  assert.deepEqual(parseCaptureScreens([2, 0, 2, -1, 'bad']), [2, 0]);
  assert.deepEqual(parseCaptureFrame([123, 1920, 1080, 'YWJj']), {
    timestamp: 123, width: 1920, height: 1080, jpeg: 'YWJj',
  });
  assert.throws(() => parseCaptureScreens([]), error => error instanceof LocalizedError && error.key === 'error.noCapture');
  assert.throws(() => parseCaptureFrame([0, 0, 1080, 'YWJj']), error => error instanceof LocalizedError && error.key === 'error.jpeg');
  assert.throws(() => parseCaptureFrame([0, 1920, 1080, '@@@']), error => error instanceof LocalizedError && error.key === 'error.jpeg');
});

test('capture RPC helpers select an explicit screen and use the fixed preview settings', async () => {
  const rpc = new CaptureRpc([[2, 0, 2], [42, 1280, 720, 'YWJj']]);
  assert.deepEqual(await readCaptureScreens(rpc), [2, 0]);
  assert.deepEqual(await readCaptureFrame(rpc, 2), { timestamp: 42, width: 1280, height: 720, jpeg: 'YWJj' });
  assert.deepEqual(rpc.requests, [
    { module: 'capture', fn: 'get_screens', params: [] },
    { module: 'capture', fn: 'get_jpg', params: [2, 65, 2] },
  ]);
  await assert.rejects(readCaptureFrame(rpc, -1), error => error instanceof LocalizedError && error.key === 'error.noCapture');
});

interface CaptureRequest { id: number; module: string; function: string; params: unknown[] }

async function captureServer() {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await once(server, 'listening');
  const requests: CaptureRequest[] = [];
  let holdFrame = false;
  let frameSeen!: () => void;
  let releaseFrame!: () => void;
  let frameWait = Promise.resolve();
  let holdStreams = false;
  let failStreams = false;
  let streamsSeen!: () => void;
  let releaseStreams!: () => void;
  let streamsWait = Promise.resolve();
  server.on('connection', socket => {
    let cipher: Rc4 | undefined;
    const send = (request: CaptureRequest, data: unknown[], errors: string[] = []): void => {
      const bytes = new TextEncoder().encode(JSON.stringify({ id: request.id, errors, data }) + '\0');
      socket.send(cipher ? cipher.crypt(bytes) : bytes);
    };
    socket.on('message', async raw => {
      const bytes = cipher ? cipher.crypt(new Uint8Array(raw as Buffer)) : new Uint8Array(raw as Buffer);
      const request = JSON.parse(new TextDecoder().decode(bytes.subarray(0, -1))) as CaptureRequest;
      requests.push(request);
      if (request.function === 'session_refresh') {
        send(request, ['capture-session']); cipher = new Rc4(new TextEncoder().encode('capture-session')); return;
      }
      if (request.module === 'capture' && request.function === 'get_screens') { send(request, [1, 0]); return; }
      if (request.module === 'capture' && request.function === 'get_jpg') {
        if (holdFrame) { frameSeen(); await frameWait; }
        send(request, [99, 1280, 720, 'YWJj']); return;
      }
      if (request.module === 'capture' && request.function === 'get_streams') {
        if (holdStreams) { streamsSeen(); await streamsWait; }
        send(request, [{ port: 1340, formats: [{ name: 'mjpeg', path: '/stream.mjpg' }],
          screens: [{ screen: 1, width: 1280, height: 720, busy: false }] }], failStreams ? ['stream failed'] : []);
        return;
      }
      if (request.module === 'buttons' && request.function === 'read') { send(request, []); return; }
      if (request.module === 'info' && request.function === 'avs') { send(request, [{ model: 'TEST' }]); return; }
      if (request.module === 'info' && request.function === 'launcher') { send(request, [{ version: 'test' }]); return; }
      if (request.module === 'info' && request.function === 'memory') { send(request, [{ mem_total: 1, mem_total_used: 1, mem_used: 1, vmem_total: 1, vmem_total_used: 1, vmem_used: 1 }]); return; }
      if (request.module === 'resize' && request.function === 'image_resize_get') {
        const scene = { offset_x: 0, offset_y: 0, scale_x: 1, scale_y: 1, keep_aspect_ratio: false, duplicate: 0 };
        send(request, [{ enabled: true, scene: 1, linear_filter: true, scenes: [scene, scene, scene, scene] }]); return;
      }
      if (request.module === 'coin' && request.function === 'get') { send(request, [0]); return; }
      send(request, []);
    });
  });
  return {
    endpoint: `ws://127.0.0.1:${(server.address() as AddressInfo).port}`,
    requests,
    holdNextFrame(): Promise<void> {
      holdFrame = true;
      frameWait = new Promise<void>(resolve => { releaseFrame = resolve; });
      return new Promise<void>(resolve => { frameSeen = resolve; });
    },
    release(): void { holdFrame = false; releaseFrame?.(); },
    holdNextStreams(fail = false): Promise<void> {
      holdStreams = true; failStreams = fail;
      streamsWait = new Promise<void>(resolve => { releaseStreams = resolve; });
      return new Promise<void>(resolve => { streamsSeen = resolve; });
    },
    releaseStreams(): void { holdStreams = false; releaseStreams?.(); },
    async close(): Promise<void> {
      releaseFrame?.(); releaseStreams?.();
      for (const client of server.clients) client.terminate();
      await new Promise<void>(resolve => server.close(() => resolve()));
    },
  };
}

test('controller lists screens, captures the selected one and drops a stale frame after disconnect', async () => {
  const server = await captureServer();
  const controller = useController();
  try {
    const profile = { id: 'capture', name: 'Capture', endpoint: server.endpoint, password: '', rememberPassword: false };
    await controller.connect(profile);
    await controller.captureScreen();
    assert.deepEqual(controller.captureScreens.value, [1, 0]);
    assert.equal(controller.selectedCaptureScreen.value, 1);
    assert.equal(controller.captureFrame.value?.width, 1280);
    assert.ok(controller.image.value.startsWith('data:image/jpeg;base64,'));

    const seen = server.holdNextFrame();
    const pending = controller.captureScreen(0);
    await seen;
    controller.disconnect();
    await pending;
    assert.equal(controller.capturing.value, false);
    const staleFrame = controller.captureFrame.value;
    assert.equal(staleFrame, undefined);
    assert.equal(controller.image.value, '');
    assert.deepEqual(server.requests.filter(request => request.function === 'get_jpg').at(-1)?.params, [0, 65, 2]);
    server.release();
    await controller.connect(profile);
    await controller.captureScreen(1);
    assert.equal(controller.selectedCaptureScreen.value, 1);
    const finalFrame = controller.captureFrame.value;
    assert.ok(finalFrame);
    assert.equal(finalFrame.height, 720);
  } finally {
    controller.disconnect();
    await server.close();
  }
});

test('stopping a stream invalidates late success and failure responses on the real encrypted client', async () => {
  const server = await captureServer(), controller = useController();
  try {
    await controller.connect({ id: 'stream', name: 'Stream', endpoint: server.endpoint, password: '', rememberPassword: false });
    for (const fail of [false, true]) {
      const seen = server.holdNextStreams(fail);
      const pending = controller.startStream();
      await seen; controller.stopStream(); server.releaseStreams(); await pending;
      assert.equal(controller.streamUrl.value, '');
      assert.equal(controller.error.value, '');
    }
  } finally { controller.disconnect(); await server.close(); }
});
