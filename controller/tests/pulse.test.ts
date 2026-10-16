import assert from 'node:assert/strict';
import { once } from 'node:events';
import type { AddressInfo } from 'node:net';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { useController } from '../src/useController';
import { Rc4 } from '../src/api/rc4';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
interface Request { id: number; module: string; function: string; params: unknown[] }

async function pulseServer(pulseError = false) {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await once(server, 'listening');
  const address = server.address() as AddressInfo;
  const requests: Request[] = [];
  let pressed = false;
  let startPulse!: () => void;
  let releasePulse!: () => void;
  const pulseStarted = new Promise<void>(resolve => { startPulse = resolve; });
  const released = new Promise<void>(resolve => { releasePulse = resolve; });

  server.on('connection', socket => {
    let cipher: Rc4 | undefined;
    let password = '';
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
        password = 'pulse-session-key';
        send(request, [password]);
        cipher = new Rc4(encoder.encode(password));
        return;
      }
      if (request.module === 'buttons' && request.function === 'pulse') {
        if (pulseError) { send(request, [], ['Unsupported function: pulse']); return; }
        pressed = true;
        startPulse();
        setTimeout(() => {
          pressed = false;
          releasePulse();
          if (socket.readyState === 1) send(request, []);
        }, 35);
        return;
      }
      if (request.module === 'buttons' && (request.function === 'write' || request.function === 'write_reset')) {
        send(request, []);
        return;
      }
      if (request.module === 'buttons' && request.function === 'read') {
        send(request, [['Service', 0, false], ['Test', 0, false]]);
        return;
      }
      if (request.module === 'info' && request.function === 'avs') {
        send(request, [{ model: 'TEST', dest: 'J', spec: 'A' }]);
        return;
      }
      if (request.module === 'info' && request.function === 'launcher') {
        send(request, [{ version: 'test' }]);
        return;
      }
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
    pulseStarted,
    released,
    get pressed() { return pressed; },
    async close(): Promise<void> {
      for (const client of server.clients) client.terminate();
      await new Promise<void>(resolve => server.close(() => resolve()));
    },
  };
}

test('pulse is one RPC and the machine-side timer releases after the browser disconnects', async () => {
  const server = await pulseServer();
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password: '', rememberPassword: false });
    const pending = controller.pulse('Service');
    await server.pulseStarted;
    assert.equal(server.pressed, true);
    controller.disconnect();
    await pending;
    await server.released;
    assert.equal(server.pressed, false);
    const pulses = server.requests.filter(request => request.module === 'buttons' && request.function === 'pulse');
    assert.equal(pulses.length, 1);
    assert.deepEqual(pulses[0]!.params, ['Service', 180]);
    assert.equal(server.requests.some(request => request.module === 'buttons' && request.function === 'write'), false);
    assert.equal(server.requests.some(request => request.module === 'buttons' && request.function === 'write_reset'), false);
  } finally {
    controller.disconnect();
    await server.close();
  }
});

test('an older server pulse error is surfaced without falling back to write or reset', async () => {
  const server = await pulseServer(true);
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password: '', rememberPassword: false });
    await controller.pulse('Service');
    assert.notEqual(controller.error.value, '');
    const pulses = server.requests.filter(request => request.module === 'buttons' && request.function === 'pulse');
    assert.equal(pulses.length, 1);
    assert.deepEqual(pulses[0]!.params, ['Service', 180]);
    assert.equal(server.requests.some(request => request.module === 'buttons' && request.function === 'write'), false);
    assert.equal(server.requests.some(request => request.module === 'buttons' && request.function === 'write_reset'), false);
  } finally {
    controller.disconnect();
    await server.close();
  }
});
