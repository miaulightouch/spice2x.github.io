import { computed, ref, shallowRef, toRaw } from 'vue';
import { ApiError, SpiceClient, asError, isRecord } from './api/client';
import type { ConnectionStatus, Rpc } from './api/client';
import { DemoApi } from './api/demo';
import { ResizeQueue, defaultScene, parseResize } from './api/resize';
import type { ResizePatch, ResizeState } from './api/resize';
import type { Profile } from './storage';

export function useController() {
  const status = ref<ConnectionStatus>('disconnected');
  const demo = ref(false);
  const message = ref('請在連線設定填入機台位址，或先試用示範模式。');
  const error = ref('');
  const busy = ref('');
  const latency = ref(0);
  const game = ref('');
  const version = ref('');
  const buttons = ref<string[]>([]);
  const coinQueue = ref<number>();
  const resize = ref<ResizeState>();
  const resizeError = ref('');
  const applying = ref(false);
  const unsaved = ref(false);
  const image = ref('');
  const imageTime = ref('');
  const capturing = ref(false);
  const configuredPassword = ref(false);
  const connected = computed(() => demo.value || status.value === 'connected');
  const api = shallowRef<Rpc>();
  let client: SpiceClient | undefined;
  let captureClient: SpiceClient | undefined;
  let activeProfile: Profile | undefined;
  let queue: ResizeQueue | undefined;
  let generation = 0;
  let revision = 0;
  let confirmed: ResizeState | undefined;

  function notify(text: string): void { message.value = text; error.value = ''; }
  function report(value: unknown): void { error.value = asError(value).message; }
  function disconnect(): void {
    ++generation;
    queue?.cancel(); queue = undefined;
    const old = client; client = undefined;
    old?.disconnect(); captureClient?.disconnect(); captureClient = undefined;
    api.value = undefined; status.value = 'disconnected'; demo.value = false;
    resize.value = undefined; resizeError.value = ''; buttons.value = []; confirmed = undefined;
    image.value = ''; imageTime.value = ''; game.value = ''; version.value = '';
    configuredPassword.value = false; applying.value = false; unsaved.value = false;
    coinQueue.value = undefined; activeProfile = undefined;
  }

  async function connect(profile: Profile): Promise<void> {
    disconnect();
    const epoch = generation;
    activeProfile = { ...profile };
    configuredPassword.value = !!profile.password;
    const next = new SpiceClient(); client = next;
    next.onStatus = (state, detail) => {
      if (client !== next) return;
      status.value = state;
      if (detail) {
        error.value = detail;
        queue?.cancel(); applying.value = false;
      }
    };
    try {
      error.value = '';
      await next.connect(profile.endpoint, profile.password);
      if (epoch !== generation) return;
      api.value = next;
      await load(epoch);
      if (epoch === generation && next.status === 'connected') notify(`已連線至 ${profile.name}`);
    } catch (value) { if (epoch === generation) report(value); }
  }

  async function startDemo(): Promise<void> {
    disconnect();
    demo.value = true; api.value = new DemoApi();
    await load(generation);
    notify('示範模式 · 所有操作只在此頁面模擬，不會送到機台。');
  }

  async function load(epoch: number): Promise<void> {
    const rpc = api.value;
    if (!rpc) return;
    const results = await Promise.allSettled([
      rpc.request('buttons', 'read'), rpc.request('info', 'avs'),
      rpc.request('info', 'launcher'), rpc.request('resize', 'image_resize_get'), rpc.request('coin', 'get'),
    ]);
    if (epoch !== generation) return;
    const [b, a, l, r, c] = results;
    if (b?.status === 'fulfilled') buttons.value = b.value.filter(v => Array.isArray(v) && typeof v[0] === 'string').map(v => (v as string[])[0]!);
    if (a?.status === 'fulfilled' && isRecord(a.value[0])) game.value = String(a.value[0].model ?? '');
    if (l?.status === 'fulfilled' && isRecord(l.value[0])) version.value = String(l.value[0].version ?? '');
    if (c?.status === 'fulfilled' && typeof c.value[0] === 'number') coinQueue.value = c.value[0];
    if (r?.status === 'fulfilled') {
      try {
        resize.value = parseResize(r.value); confirmed = structuredClone(toRaw(resize.value));
        installQueue(rpc, epoch);
      } catch (value) { resizeError.value = asError(value).message; }
    } else if (r?.status === 'rejected') {
      resizeError.value = `畫面校正無法使用：${asError(r.reason).message}`;
    }
    if (client) latency.value = client.latency;
  }

  function installQueue(rpc: Rpc, epoch: number): void {
    queue = new ResizeQueue(async patch => {
      const sentRevision = revision;
      const state = parseResize(await rpc.request('resize', 'image_resize_set', [patch]));
      if (epoch !== generation) return;
      confirmed = state;
      if (sentRevision === revision) { resize.value = structuredClone(state); applying.value = false; }
      unsaved.value = true;
      if (client) latency.value = client.latency;
    }, value => {
      if (epoch !== generation) return;
      applying.value = false;
      if (confirmed) resize.value = structuredClone(confirmed);
      report(value);
    });
  }

  function updateResize(patch: ResizePatch): void {
    if (!connected.value || !resize.value || !queue) return;
    const state = resize.value;
    const target = patch.scene ?? state.scene;
    const scene = state.scenes[target - 1]!;
    const { enabled, linear_filter, scene: _, ...settings } = patch;
    Object.assign(scene, settings);
    if (scene.keep_aspect_ratio) {
      if (patch.scale_y !== undefined && patch.scale_x === undefined) scene.scale_x = scene.scale_y;
      else scene.scale_y = scene.scale_x;
    }
    if (enabled !== undefined) state.enabled = enabled;
    if (linear_filter !== undefined) state.linear_filter = linear_filter;
    ++revision; applying.value = true; error.value = '';
    queue.update({ ...patch, scene: target });
  }
  async function selectScene(scene: number): Promise<void> {
    if (!resize.value || !api.value) return;
    await action('scene', async () => {
      await queue?.flush();
      const state = parseResize(await api.value!.request('resize', 'image_resize_set', [{ scene }]));
      resize.value = state; confirmed = structuredClone(state);
      notify(`已切換場景 ${scene}`);
    });
  }
  async function readResize(): Promise<void> {
    await action('read', async () => {
      const rpc = api.value!;
      await queue?.flush();
      const state = parseResize(await rpc.request('resize', 'image_resize_get'));
      resize.value = state; confirmed = structuredClone(state); resizeError.value = '';
      if (!queue) installQueue(rpc, generation);
      notify('已讀取機台目前的畫面設定');
    });
  }
  async function saveResize(): Promise<void> {
    await action('save', async () => {
      if (!demo.value && !configuredPassword.value) throw new Error('寫入設定檔需要在 spice2x 啟用 API 密碼，並使用相同密碼連線。');
      await queue?.flush();
      await api.value!.request('resize', 'image_resize_save');
      unsaved.value = false;
      notify(demo.value ? '示範設定已儲存，未寫入機台。' : '已將四組場景寫入機台設定檔。');
    });
  }

  async function action(key: string, fn: () => Promise<void>): Promise<void> {
    if (busy.value) return;
    if (!connected.value || !api.value) { report(new Error('請先連線至機台。')); return; }
    const epoch = generation;
    busy.value = key; error.value = '';
    try { await fn(); }
    catch (value) { if (epoch === generation) report(value); }
    finally { busy.value = ''; }
  }
  function exactButton(label: string): string | undefined { return buttons.value.find(n => n.toLowerCase() === label.toLowerCase()); }
  async function pulse(label: string): Promise<void> {
    await action(label, async () => {
      const name = exactButton(label);
      if (!name) throw new Error(`機台沒有提供 ${label} 按鍵。`);
      const rpc = api.value!;
      await rpc.request('buttons', 'write', [[name, true]]);
      try { await new Promise(resolve => setTimeout(resolve, 180)); }
      finally { await rpc.request('buttons', 'write_reset', [[name]]); }
      notify(`${label.toUpperCase()} 已按下並放開`);
    });
  }
  async function releaseButtons(): Promise<void> {
    await action('release', async () => { await api.value!.request('buttons', 'write_reset'); notify('已解除所有按鍵覆寫'); });
  }
  async function insertCoin(): Promise<void> {
    await action('coin', async () => {
      await api.value!.request('coin', 'insert', [1]);
      notify('已送出 1 枚投幣');
      try { const data = await api.value!.request('coin', 'get'); if (typeof data[0] === 'number') coinQueue.value = data[0]; } catch { /* Insertion already succeeded; never replay it. */ }
    });
  }
  async function insertCard(unit: number, number: string): Promise<void> {
    await action('card', async () => { await api.value!.request('card', 'insert', [unit, number]); notify(`已送出卡片至 P${unit + 1}`); });
  }
  async function getMachineCards(): Promise<unknown[]> {
    if (!connected.value || !api.value) throw new Error('請先連線至機台。');
    const epoch = generation;
    const data = await api.value.request('card', 'get_cards');
    if (epoch !== generation) throw new Error('連線已變更，請重新讀取卡片。');
    return data;
  }
  async function keypad(unit: number, text: string): Promise<void> {
    await action('keypad', async () => { await api.value!.request('keypads', 'write', [unit, text]); notify('已送出數字鍵'); });
  }

  async function capture(): Promise<void> {
    if (!connected.value || capturing.value) return;
    if (demo.value) { notify('示範模式僅顯示校正格線。'); return; }
    if (!activeProfile) return;
    const epoch = generation;
    capturing.value = true; error.value = '';
    const next = new SpiceClient(15000); captureClient = next;
    try {
      await next.connect(activeProfile.endpoint, activeProfile.password);
      const screens = await next.request('capture', 'get_screens');
      const screen = screens.find(v => Number.isInteger(v) && Number(v) >= 0);
      if (screen === undefined) throw new Error('機台目前沒有可擷取的畫面。');
      const data = await next.request('capture', 'get_jpg', [screen, 65, 2]);
      if (typeof data[3] !== 'string' || !/^[A-Za-z0-9+/=\r\n]+$/.test(data[3])) throw new Error('機台未提供有效 JPEG；這個版本可能沒有畫面擷取功能。');
      if (epoch === generation) {
        image.value = 'data:image/jpeg;base64,' + data[3];
        imageTime.value = new Date().toLocaleTimeString('zh-TW', { hour12: false });
        notify('畫面已更新；畫面擷取使用獨立連線。');
      }
    } catch (value) { if (epoch === generation) report(value instanceof ApiError ? new Error(`畫面擷取不可用：${value.message}`) : value); }
    finally { next.disconnect(); if (captureClient === next) captureClient = undefined; capturing.value = false; }
  }

  return { status, demo, message, error, busy, latency, game, version, buttons, coinQueue, resize, resizeError,
    applying, unsaved, image, imageTime, capturing, configuredPassword, connected,
    connect, startDemo, disconnect, notify, report, updateResize, selectScene, readResize, saveResize,
    pulse, exactButton, releaseButtons, insertCoin, insertCard, getMachineCards, keypad, capture,
    currentScene: computed(() => resize.value?.scenes[(resize.value?.scene ?? 1) - 1] ?? defaultScene()) };
}
