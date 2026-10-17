import assert from 'node:assert/strict';
import { once } from 'node:events';
import type { AddressInfo } from 'node:net';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { parsePatches, normalizePatchUrl } from '../src/api/features';
import { useController } from '../src/useController';
import { Rc4 } from '../src/api/rc4';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const password = 'patch-import-password';
const importUrl = 'https://patch.example/source';

interface Request { id: number; module: string; function: string; params: unknown[] }
interface PatchServerOptions { canImport?: boolean; importDelay?: number; importError?: string; password?: string }

function patchState(options: PatchServerOptions, importUrlValue = '', importErrors = ''): unknown[] {
  return [{ game: 'TEST', auto_apply: false, dirty: false, standalone: true,
    can_import: options.canImport ?? true, import_url: importUrlValue, recent_urls: importUrlValue ? [importUrlValue] : [], import_errors: importErrors, patches: [] }];
}

async function patchServer(options: PatchServerOptions = {}) {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await once(server, 'listening');
  const requests: Array<Request & { connection: number }> = [];
  const initialPassword = options.password ?? password;
  let resolveImportStarted!: () => void;
  const started = new Promise<void>(resolve => { resolveImportStarted = resolve; });
  let connectionCount = 0;
  let importedUrl = '';
  let importErrors = '';

  server.on('connection', socket => {
    const connection = ++connectionCount;
    let cipher: Rc4 | undefined = initialPassword ? new Rc4(encoder.encode(initialPassword)) : undefined;
    const send = (request: Request, data: unknown[], errors: string[] = []): void => {
      const bytes = encoder.encode(JSON.stringify({ id: request.id, errors, data }) + '\0');
      if (socket.readyState === 1) socket.send(cipher ? cipher.crypt(bytes) : bytes);
    };
    socket.on('message', (raw, binary) => {
      assert.equal(binary, true);
      const bytes = cipher ? cipher.crypt(new Uint8Array(raw as Buffer)) : new Uint8Array(raw as Buffer);
      assert.equal(bytes.at(-1), 0);
      const request = JSON.parse(decoder.decode(bytes.subarray(0, -1))) as Request;
      requests.push({ ...request, connection });
      if (request.function === 'session_refresh') {
        const sessionKey = `session-${connection}`;
        send(request, [sessionKey]);
        cipher = new Rc4(encoder.encode(sessionKey));
        return;
      }
      if (request.module === 'patches' && request.function === 'get') {
        send(request, patchState(options, importedUrl, importErrors));
        return;
      }
      if (request.module === 'patches' && request.function === 'import_url') {
        importedUrl = String(request.params[0]);
        resolveImportStarted();
        const delay = options.importDelay ?? 0;
        setTimeout(() => {
          if (options.importError) {
            importErrors = options.importError;
            send(request, [], [options.importError]);
          } else send(request, patchState(options, importedUrl));
        }, delay);
        return;
      }
      if (request.module === 'info' && request.function === 'avs') { send(request, [{ model: 'TEST', dest: 'J', spec: 'A' }]); return; }
      if (request.module === 'info' && request.function === 'launcher') { send(request, [{ version: 'test' }]); return; }
      if (request.module === 'info' && request.function === 'memory') {
        send(request, [{ mem_total: 8, mem_total_used: 2, mem_used: 1, vmem_total: 16, vmem_total_used: 4, vmem_used: 2 }]); return;
      }
      if (request.module === 'buttons' && request.function === 'read') { send(request, [['Service', 0, false]]); return; }
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
    importStarted: started,
    get connectionCount() { return connectionCount; },
    async close(): Promise<void> {
      for (const client of server.clients) client.terminate();
      server.close();
      await once(server, 'close');
    },
  };
}

test('patch source URLs are normalized and reject unsafe bases', () => {
  assert.equal(normalizePatchUrl(' https://patch.example/source '), importUrl);
  assert.equal(normalizePatchUrl('http://patch.example:8080/root/'), 'http://patch.example:8080/root/');
  for (const value of [
    'ftp://patch.example/source', '//patch.example/source', 'https://user:pass@patch.example/source',
    'https://patch.example/source?channel=stable', 'https://patch.example/source#stable',
    'https://patch.example/source?', 'https://patch.example/source#',
    'https://patch.example/source path', 'https://patch.example/source\npath',
    'https://patch.example/' + 'a'.repeat(4096),
  ]) assert.throws(() => normalizePatchUrl(value));
});

test('patch metadata keeps old hosts compatible and validates import fields', () => {
  const patch = { id: 'p', name: 'Patch', description: '', caution: '', group: '', type: 'memory', enabled: false,
    status: 'disabled', error: '', unverified: false, choices: [], selected: '' };
  const old = parsePatches([{ game: 'TEST', auto_apply: false, dirty: false, standalone: false, patches: [patch] }]);
  assert.deepEqual(old.import_url, '');
  assert.deepEqual(old.recent_urls, []);
  assert.equal(old.can_import, false);
  assert.equal(old.import_errors, '');
  const current = parsePatches([{ game: 'TEST', auto_apply: false, dirty: false, standalone: true,
    can_import: true, import_url: importUrl, recent_urls: [importUrl], import_errors: 'stale', patches: [patch] }]);
  assert.equal(current.can_import, true);
  assert.equal(current.import_url, importUrl);
  assert.deepEqual(current.recent_urls, [importUrl]);
  assert.equal(current.import_errors, 'stale');
  for (const field of [
    { can_import: 'yes' }, { import_url: 1 }, { recent_urls: 'bad' }, { recent_urls: [1] }, { import_errors: 1 },
  ]) assert.throws(() => parsePatches([{ game: 'TEST', auto_apply: false, dirty: false, standalone: false, ...field, patches: [] }]));
});

test('native import uses a separate encrypted connection and survives beyond the default RPC timeout', { timeout: 30_000 }, async () => {
  const server = await patchServer({ importDelay: 8100 });
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password, rememberPassword: false });
    await controller.refreshPatches();
    const pending = controller.importPatches(importUrl);
    await server.importStarted;
    assert.equal(server.connectionCount, 2);
    await controller.readStatus();
    assert.equal(controller.game.value, 'TEST');
    assert.equal(controller.version.value, 'test');
    assert.equal(await pending, true);
    assert.equal(controller.patches.value?.import_url, importUrl);
    assert.ok(server.requests.some(request => request.connection === 1 && request.module === 'info' && request.function === 'avs'));
    assert.equal(server.requests.filter(request => request.module === 'patches' && request.function === 'import_url').length, 1);
    const importRequest = server.requests.find(request => request.module === 'patches' && request.function === 'import_url');
    assert.equal(importRequest?.connection, 2);
  } finally {
    controller.disconnect();
    await server.close();
  }
});

test('an API import error is refreshed once and is never retried', async () => {
  const server = await patchServer({ importError: 'remote import rejected' });
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password, rememberPassword: false });
    await controller.refreshPatches();
    assert.equal(await controller.importPatches(importUrl), false);
    assert.match(controller.error.value, /remote import rejected/);
    assert.equal(server.requests.filter(request => request.module === 'patches' && request.function === 'import_url').length, 1);
    assert.equal(server.requests.filter(request => request.connection === 1 && request.module === 'patches' && request.function === 'get').length, 2);
    assert.equal(controller.patches.value?.import_errors, 'remote import rejected');
  } finally {
    controller.disconnect();
    await server.close();
  }
});

test('can_import false rejects before opening an import request', async () => {
  const server = await patchServer({ canImport: false });
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password, rememberPassword: false });
    await controller.refreshPatches();
    assert.equal(await controller.importPatches(importUrl), false);
    assert.equal(server.connectionCount, 1);
    assert.equal(server.requests.some(request => request.module === 'patches' && request.function === 'import_url'), false);
    assert.equal(controller.error.value, '此主機無法匯入 URL。');
  } finally {
    controller.disconnect();
    await server.close();
  }
});

test('native import requires an API password before opening its second connection', async () => {
  const server = await patchServer({ password: '' });
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password: '', rememberPassword: false });
    await controller.refreshPatches();
    assert.equal(await controller.importPatches(importUrl), false);
    assert.equal(server.connectionCount, 1);
    assert.equal(server.requests.some(request => request.module === 'patches' && request.function === 'import_url'), false);
    assert.match(controller.error.value, /API 密碼/);
  } finally {
    controller.disconnect();
    await server.close();
  }
});

test('a disconnected import cannot publish its stale result', async () => {
  const server = await patchServer({ importDelay: 40 });
  const controller = useController();
  try {
    await controller.connect({ id: 'test', name: 'test', endpoint: server.endpoint, password, rememberPassword: false });
    await controller.refreshPatches();
    const pending = controller.importPatches(importUrl);
    await server.importStarted;
    const messageBeforeDisconnect = controller.message.value;
    controller.disconnect();
    assert.equal(await pending, false);
    await new Promise(resolve => setTimeout(resolve, 60));
    assert.equal(controller.patches.value, undefined);
    assert.equal(controller.message.value, messageBeforeDisconnect);
    assert.equal(controller.error.value, '');
  } finally {
    controller.disconnect();
    await server.close();
  }
});
