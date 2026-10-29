import assert from 'node:assert/strict';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { test } from 'node:test';
import { createRenderer, createSSRApp, h, nextTick, reactive, ref } from 'vue';
import { renderToString } from 'vue/server-renderer';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { locale } from '../src/i18n';
import { selectUpdateFiles } from '../src/api/update';
import type { UpdateStatus } from '../src/api/update';
import type { useController } from '../src/useController';

type Controller = ReturnType<typeof useController>;

interface HostNode {
  type: string;
  children: HostNode[];
  props: Record<string, unknown>;
  text: string;
  parent?: HostNode;
}

function hostRenderer() {
  return createRenderer<HostNode, HostNode>({
    patchProp(node, key, _previous, value) { node.props[key] = value; },
    insert(node, parent, anchor) {
      node.parent = parent;
      const index = anchor ? parent.children.indexOf(anchor) : -1;
      if (index < 0) parent.children.push(node); else parent.children.splice(index, 0, node);
    },
    remove(node) { if (node.parent) node.parent.children = node.parent.children.filter(child => child !== node); },
    createElement(type) { return { type, children: [], props: {}, text: '' }; },
    createText(text) { return { type: '#text', children: [], props: {}, text }; },
    createComment(text) { return { type: '#comment', children: [], props: {}, text }; },
    setText(node, text) { node.text = text; },
    setElementText(node, text) { node.text = text; node.children = []; },
    parentNode(node) { return node.parent ?? null; },
    nextSibling(node) {
      const siblings = node.parent?.children ?? []; const index = siblings.indexOf(node);
      return index >= 0 ? siblings[index + 1] ?? null : null;
    },
    querySelector() { return null; },
    setScopeId() {},
    cloneNode(node) { return { ...node, children: [...node.children], props: { ...node.props } }; },
    insertStaticContent() {
      const start: HostNode = { type: '#text', children: [], props: {}, text: '' };
      const end: HostNode = { type: '#text', children: [], props: {}, text: '' };
      return [start, end];
    },
  });
}

function findNodes(root: HostNode, predicate: (node: HostNode) => boolean): HostNode[] {
  return [ ...(predicate(root) ? [root] : []), ...root.children.flatMap(child => findNodes(child, predicate)) ];
}

function nodeText(node: HostNode): string { return node.text + node.children.map(nodeText).join(''); }

async function loadClientSystemUpdate(server: Awaited<ReturnType<typeof createServer>>) {
  const transformed = await server.transformRequest('/src/components/SystemUpdate.vue');
  assert.ok(transformed?.code, 'Vite should compile the client component');
  const sourceRoot = fileURLToPath(new URL('../src/', import.meta.url));
  const vueModule = pathToFileURL(fileURLToPath(new URL('../node_modules/vue/index.mjs', import.meta.url))).href;
  const exportHelper = `data:text/javascript,${encodeURIComponent('export default (sfc, props) => { const target = sfc.__vccOpts || sfc; for (const [key, value] of props) target[key] = value; return target; }')}`;
  const moduleSource = transformed.code
    .replaceAll(/"\/node_modules\/(?:\.vite\/deps\/vue\.js\?[^\"]+|vue\/[^\"]+)"/g, JSON.stringify(vueModule))
    .replaceAll('"/src/i18n.ts"', JSON.stringify(pathToFileURL(`${sourceRoot}i18n.ts`).href))
    .replaceAll('"/src/api/update.ts"', JSON.stringify(pathToFileURL(`${sourceRoot}api/update.ts`).href))
    .replaceAll('"/src/api/updateFlow.ts"', JSON.stringify(pathToFileURL(`${sourceRoot}api/updateFlow.ts`).href))
    .replaceAll('"/@id/__x00__plugin-vue:export-helper"', JSON.stringify(exportHelper));
  const moduleUrl = `data:text/javascript;base64,${Buffer.from(moduleSource).toString('base64')}`;
  return (await import(moduleUrl)).default;
}

async function settle(): Promise<void> {
  for (let i = 0; i < 8; i++) { await nextTick(); await Promise.resolve(); }
}

function updateFileInput(root: HostNode, file: File): void {
  const input = findNodes(root, node => node.type === 'input')[0]!;
  (input.props.onChange as (event: Event) => void)({ target: { files: [file] } } as unknown as Event);
}

function clickButton(root: HostNode, text: string | RegExp): void {
  const button = findNodes(root, node => node.type === 'button' && (typeof text === 'string' ? nodeText(node).includes(text) : text.test(nodeText(node))))[0];
  assert.ok(button, `button ${text}`);
  (button.props.onClick as (event: Event) => void)({} as Event);
}

function flowController(status: UpdateStatus, newCrc32: number, readyRead = 3) {
  const connected = ref(true); const connection = ref<'connected' | 'disconnected'>('connected');
  const updateStatus = ref<UpdateStatus>({ ...status }); const updateError = ref(''); const busy = ref(''); const version = ref('new');
  let statusReads = 0; let beginCalls = 0; let applyCalls = 0; let reconnectCalls = 0; const notifications: string[] = [];
  const controller = {
    status: connection, connected, demo: ref(false), configuredPassword: ref(true), busy, updateStatus, updateError, version,
    async readUpdateStatus() {
      statusReads++;
      updateStatus.value = { ...status, executable_crc32: statusReads >= readyRead ? newCrc32 : status.executable_crc32, update_id: statusReads >= readyRead ? `upload-${beginCalls}` : status.update_id };
      return updateStatus.value;
    },
    async beginUpdate() { beginCalls++; return 'upload-' + beginCalls; },
    async updateChunk(_id: string, _name: string, offset: number, data: string) { return offset + Uint8Array.from(atob(data), char => char.charCodeAt(0)).length; },
    async finishUpdate() { return { files: [{ name: status.executable, size: 3, signature: { status: 'unsigned' as const, signer: '', detail: '' } }] }; },
    async cancelUpdate() { return true; },
    async applyUpdate() { applyCalls++; return true; },
    async reconnect() { reconnectCalls++; connected.value = true; connection.value = 'connected'; },
    notify(value: unknown) { notifications.push(String(value)); },
  } as unknown as Controller;
  return { controller, connected, connection, updateStatus, get beginCalls() { return beginCalls; }, get applyCalls() { return applyCalls; }, get reconnectCalls() { return reconnectCalls; }, notifications };
}

function fakeController(status: 'connected' | 'disconnected', updateStatus: unknown, updateError = ''): Controller {
  return {
    status: ref(status), connected: ref(status === 'connected'), demo: ref(false), configuredPassword: ref(true), busy: ref(''),
    updateStatus: ref(updateStatus), updateError: ref(updateError), version: ref('old'),
    readUpdateStatus: async () => undefined,
  } as unknown as Controller;
}

test('system update UI gates disconnected and passwordless sessions before exposing upload controls', async () => {
  const server = await createServer({
    root: fileURLToPath(new URL('..', import.meta.url)), configFile: false,
    plugins: [vue()], optimizeDeps: { noDiscovery: true }, server: { middlewareMode: true, hmr: false, ws: false }, appType: 'custom',
  });
  try {
    locale.value = 'en';
    const { default: SystemUpdate } = await server.ssrLoadModule('/src/components/SystemUpdate.vue');
    const status = { version: 'v', architecture: 'x64', xp: false, executable: 'spice64.exe', executable_crc32: 1, update_id: '', max_file_bytes: 64, max_total_bytes: 128, chunk_bytes: 32 };
    const disconnected = await renderToString(createSSRApp(SystemUpdate, { controller: fakeController('disconnected', undefined), active: true, connected: false, demo: false, configuredPassword: false }));
    assert.match(disconnected, /Connect to update|連線後更新/);
    assert.doesNotMatch(disconnected, /type="file"/);
    const passwordless = await renderToString(createSSRApp(SystemUpdate, { controller: fakeController('connected', undefined), active: true, connected: true, demo: false, configuredPassword: false }));
    assert.match(passwordless, /API secret required|需要 API 密碼/);
    const ready = await renderToString(createSSRApp(SystemUpdate, { controller: fakeController('connected', status), active: true, connected: true, demo: false, configuredPassword: true }));
    assert.match(ready, /type="file"/);
    assert.match(ready, /Select release files|選擇版本檔案/);
    assert.doesNotMatch(ready, /Update and restart/);
  } finally { locale.value = 'zh-TW'; await server.close(); }
});

test('system update clears a socket upload id before allowing a fresh upload after reconnect', async () => {
  const server = await createServer({
    root: fileURLToPath(new URL('..', import.meta.url)), configFile: false,
    plugins: [vue()], optimizeDeps: { noDiscovery: true }, server: { middlewareMode: true, hmr: false, ws: false }, appType: 'custom',
  });
  try {
    const SystemUpdate = await loadClientSystemUpdate(server);
    const status: UpdateStatus = { version: 'old', architecture: 'x64', xp: false, executable: 'spice64.exe', executable_crc32: 0, update_id: '', max_file_bytes: 64, max_total_bytes: 128, chunk_bytes: 32 };
    const flow = flowController(status, 0);
    const props = reactive({ active: true, connected: true, demo: false, configuredPassword: true });
    const root: HostNode = { type: 'root', children: [], props: {}, text: '' };
    const app = hostRenderer().createApp({ setup: () => () => h(SystemUpdate, { controller: flow.controller, ...props }) });
    app.mount(root);
    await settle();

    const file = new File([new Uint8Array([1, 2, 3])], 'spice64.exe');
    updateFileInput(root, file);
    await settle();
    clickButton(root, /Upload files|上傳檔案|上传文件|ファイルをアップロード|파일 업로드/);
    await settle();
    assert.equal(flow.beginCalls, 1);
    assert.equal(findNodes(root, node => node.type === 'input')[0]!.props.disabled, true);

    clickButton(root, /Cancel|取消|キャンセル|취소/);
    await settle();
    assert.equal(findNodes(root, node => node.type === 'input')[0]!.props.disabled, false);
    clickButton(root, /Upload files|上傳檔案|上传文件|ファイルをアップロード|파일 업로드/);
    await settle();
    assert.equal(flow.beginCalls, 2);
    assert.equal(findNodes(root, node => node.type === 'input')[0]!.props.disabled, true);

    props.connected = false;
    flow.connected.value = false;
    flow.connection.value = 'disconnected';
    await settle();
    props.connected = true;
    flow.connected.value = true;
    flow.connection.value = 'connected';
    await settle();
    assert.equal(findNodes(root, node => node.type === 'input')[0]!.props.disabled, false);

    updateFileInput(root, file);
    await settle();
    clickButton(root, /Upload files|上傳檔案|上传文件|ファイルをアップロード|파일 업로드/);
    await settle();
    assert.equal(flow.beginCalls, 3);
    app.unmount();
  } finally { await server.close(); }
});

test('system update applies once and verifies the new executable crc after an old reconnect status', async () => {
  const server = await createServer({
    root: fileURLToPath(new URL('..', import.meta.url)), configFile: false,
    plugins: [vue()], optimizeDeps: { noDiscovery: true }, server: { middlewareMode: true, hmr: false, ws: false }, appType: 'custom',
  });
  const originalSetTimeout = globalThis.setTimeout;
  try {
    const SystemUpdate = await loadClientSystemUpdate(server);
    const status: UpdateStatus = { version: 'old', architecture: 'x64', xp: false, executable: 'spice64.exe', executable_crc32: 0, update_id: '', max_file_bytes: 64, max_total_bytes: 128, chunk_bytes: 32 };
    const bytes = new Uint8Array([1, 2, 3]);
    const expectedCrc32 = selectUpdateFiles('spice64.exe', bytes, status)[0]!.crc32;
    const flow = flowController(status, expectedCrc32);
    const props = reactive({ active: true, connected: true, demo: false, configuredPassword: true });
    const root: HostNode = { type: 'root', children: [], props: {}, text: '' };
    const app = hostRenderer().createApp({ setup: () => () => h(SystemUpdate, { controller: flow.controller, ...props }) });
    app.mount(root);
    await settle();
    const file = new File([bytes], 'spice64.exe');
    updateFileInput(root, file);
    await settle();
    clickButton(root, /Upload files|上傳檔案|上传文件|ファイルをアップロード|파일 업로드/);
    await settle();
    assert.equal(flow.beginCalls, 1);

    globalThis.setTimeout = ((callback: TimerHandler) => {
      if (typeof callback === 'function') callback();
      return 0 as unknown as ReturnType<typeof setTimeout>;
    }) as unknown as typeof setTimeout;
    clickButton(root, /Update and restart|更新並重新啟動|更新并重新启动|更新して再起動|업데이트 및 다시 시작/);
    await settle();
    assert.equal(flow.applyCalls, 1);
    assert.ok(flow.reconnectCalls >= 2);
    assert.equal(flow.notifications.length, 1);
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    await server.close();
  }
});

test('system update waits for the matching upload id when the executable crc is already equal', async () => {
  const server = await createServer({
    root: fileURLToPath(new URL('..', import.meta.url)), configFile: false,
    plugins: [vue()], optimizeDeps: { noDiscovery: true }, server: { middlewareMode: true, hmr: false, ws: false }, appType: 'custom',
  });
  const originalSetTimeout = globalThis.setTimeout;
  const timers: Array<() => void> = [];
  try {
    const SystemUpdate = await loadClientSystemUpdate(server);
    const bytes = new Uint8Array([1, 2, 3]);
    const baseStatus: UpdateStatus = { version: 'old', architecture: 'x64', xp: false, executable: 'spice64.exe', executable_crc32: 0, update_id: '', max_file_bytes: 64, max_total_bytes: 128, chunk_bytes: 32 };
    const expectedCrc32 = selectUpdateFiles('spice64.exe', bytes, baseStatus)[0]!.crc32;
    const status: UpdateStatus = { ...baseStatus, executable_crc32: expectedCrc32 };
    const flow = flowController(status, expectedCrc32, 3);
    const props = reactive({ active: true, connected: true, demo: false, configuredPassword: true });
    const root: HostNode = { type: 'root', children: [], props: {}, text: '' };
    const app = hostRenderer().createApp({ setup: () => () => h(SystemUpdate, { controller: flow.controller, ...props }) });
    app.mount(root);
    await settle();
    updateFileInput(root, new File([bytes], 'spice64.exe'));
    await settle();
    clickButton(root, /Upload files|上傳檔案|上传文件|ファイルをアップロード|파일 업로드/);
    await settle();

    globalThis.setTimeout = ((callback: TimerHandler) => {
      if (typeof callback === 'function') timers.push(callback as () => void);
      return timers.length as unknown as ReturnType<typeof setTimeout>;
    }) as unknown as typeof setTimeout;
    clickButton(root, /Update and restart|更新並重新啟動|更新并重新启动|更新して再起動|업데이트 및 다시 시작/);
    await settle();
    assert.equal(timers.length, 1);
    timers.shift()!();
    await settle();
    assert.equal(flow.reconnectCalls, 1);
    assert.equal(flow.updateStatus.value.executable_crc32, expectedCrc32);
    assert.equal(flow.updateStatus.value.update_id, '');
    assert.equal(flow.notifications.length, 0);
    assert.equal(timers.length, 1);

    timers.shift()!();
    await settle();
    assert.equal(flow.applyCalls, 1);
    assert.equal(flow.reconnectCalls, 2);
    assert.equal(flow.updateStatus.value.update_id, 'upload-1');
    assert.equal(flow.notifications.length, 1);
    app.unmount();
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    await server.close();
  }
});
