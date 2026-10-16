import { locale, msg, textRef, errorText, LocalizedError } from './i18n';
import { computed, ref, shallowRef, toRaw } from 'vue';
import { ApiError, SpiceClient, asError, isRecord } from './api/client';
import type { ConnectionStatus, Rpc } from './api/client';
import { DemoApi } from './api/demo';
import { ResizeQueue, defaultScene, parseResize } from './api/resize';
import type { ResizePatch, ResizeState } from './api/resize';
import type { TextValue } from './i18n';
import type { Profile } from './storage';

export function useController() {
  const status = ref<ConnectionStatus>('disconnected');
  const demo = ref(false);
  const message = textRef(msg('msg.initial'));
  const error = textRef();
  const busy = ref('');
  const latency = ref(0);
  const game = ref('');
  const version = ref('');
  const buttons = ref<string[]>([]);
  const coinQueue = ref<number>();
  const resize = ref<ResizeState>();
  const resizeError = textRef();
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
  let resizeApplyError: Error | undefined;

  function notify(text: TextValue): void { message.value = text; error.value = ''; }
  function report(value: unknown): void { error.value = errorText(value); }
  function disconnect(): void {
    ++generation;
    queue?.cancel(); queue = undefined;
    const old = client; client = undefined;
    old?.disconnect(); captureClient?.disconnect(); captureClient = undefined;
    api.value = undefined; status.value = 'disconnected'; demo.value = false;
    resize.value = undefined; resizeError.value = ''; buttons.value = []; confirmed = undefined;
    image.value = ''; imageTime.value = ''; game.value = ''; version.value = '';
    configuredPassword.value = false; applying.value = false; unsaved.value = false;
    coinQueue.value = undefined; activeProfile = undefined; resizeApplyError = undefined;
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
      if (epoch === generation && next.status === 'connected') notify(msg('msg.connected', { name: profile.name }));
    } catch (value) { if (epoch === generation) report(value); }
  }

  async function startDemo(): Promise<void> {
    disconnect();
    demo.value = true; api.value = new DemoApi();
    await load(generation);
    notify(msg('msg.demo'));
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
      } catch (value) { resizeError.value = errorText(value); }
    } else if (r?.status === 'rejected') {
      resizeError.value = msg('error.resizeUnavailable', { detail: errorText(r.reason) });
    }
    if (client) latency.value = client.latency;
  }

  function installQueue(rpc: Rpc, epoch: number): void {
    queue = new ResizeQueue(async patch => {
      const sentRevision = revision;
      const state = parseResize(await rpc.request('resize', 'image_resize_set', [patch]));
      if (epoch !== generation) return;
      confirmed = state; resizeApplyError = undefined;
      if (sentRevision === revision) { resize.value = structuredClone(state); applying.value = false; }
      unsaved.value = true;
      if (client) latency.value = client.latency;
    }, value => {
      if (epoch !== generation) return;
      applying.value = false; resizeApplyError = asError(value);
      if (confirmed) resize.value = structuredClone(confirmed);
      report(value);
    });
  }

  function updateResize(patch: ResizePatch): void {
    if (!connected.value || !resize.value || !queue) return;
    // Scene edits activate their transform; an explicit disable still wins.
    if (patch.enabled === undefined && ['offset_x', 'offset_y', 'scale_x', 'scale_y', 'keep_aspect_ratio', 'duplicate'].some(key => key in patch)) {
      patch = { ...patch, enabled: true };
    }
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
    const queued: ResizePatch = { ...patch, scene: target };
    // A later edit to either axis must replace BOTH pending scale fields. Otherwise
    // coalescing X then Y can send unequal values that the native API rejects.
    if (scene.keep_aspect_ratio && (patch.scale_x !== undefined || patch.scale_y !== undefined || patch.keep_aspect_ratio === true)) {
      queued.scale_x = scene.scale_x;
      queued.scale_y = scene.scale_y;
    }
    queue.update(queued);
  }
  async function selectScene(scene: number): Promise<void> {
    if (!resize.value || !api.value) return;
    await action('scene', async () => {
      await queue?.flush();
      const state = parseResize(await api.value!.request('resize', 'image_resize_set', [{ scene, enabled: true }]));
      resize.value = state; confirmed = structuredClone(state); unsaved.value = true; resizeApplyError = undefined;
      notify(msg('msg.sceneSelected', { n: scene }));
    });
  }
  async function readResize(): Promise<void> {
    await action('read', async () => {
      const rpc = api.value!;
      await queue?.flush();
      const state = parseResize(await rpc.request('resize', 'image_resize_get'));
      resize.value = state; confirmed = structuredClone(state); resizeError.value = ''; resizeApplyError = undefined;
      if (!queue) installQueue(rpc, generation);
      notify(msg('msg.resizeRead'));
    });
  }
  async function saveResize(): Promise<void> {
    await action('save', async () => {
      if (!demo.value && !configuredPassword.value) throw new LocalizedError('error.savePassword');
      await queue?.flush();
      if (resizeApplyError) throw new LocalizedError('error.apply', { detail: errorText(resizeApplyError) });
      const expected = confirmed;
      await api.value!.request('resize', 'image_resize_save');
      const state = parseResize(await api.value!.request('resize', 'image_resize_get'));
      if (JSON.stringify(state) !== JSON.stringify(expected)) throw new LocalizedError('error.saveMismatch');
      unsaved.value = false;
      resize.value = state; confirmed = structuredClone(state);
      notify(msg('msg.savedStatus', {
        summary: msg(demo.value ? 'msg.demoSaved' : 'msg.resizeSaved'),
        state: state.enabled ? msg('msg.resizeEnabled', { n: state.scene }) : msg('msg.resizeDisabled'),
      }));
    });
  }

  async function machineControl(command: 'restart' | 'exit' | 'reboot' | 'shutdown'): Promise<void> {
    await action(command, async () => {
      if (!demo.value && !configuredPassword.value) throw new LocalizedError('error.controlPassword');
      const epoch = generation;
      const rpc = api.value!;
      const simulated = demo.value;
      const label = msg(({ restart: 'action.restart', exit: 'action.exit', reboot: 'action.reboot', shutdown: 'action.shutdown' } as const)[command]);
      await queue?.flush();
      if (epoch !== generation) return;
      try {
        await rpc.request('control', command);
      } catch (value) {
        if (epoch !== generation) return;
        if (value instanceof ApiError) throw value;
        // Process termination can close the API before a response. Never retry.
        disconnect();
        message.value = msg('msg.actionUnknown', { action: label });
        error.value = '';
        return;
      }
      if (epoch !== generation) return;
      if (!simulated) disconnect();
      notify(msg(simulated ? 'msg.actionDemo' : 'msg.actionAccepted', { action: label }));
    });
  }

  async function action(key: string, fn: () => Promise<void>): Promise<void> {
    if (busy.value) return;
    if (!connected.value || !api.value) { report(new LocalizedError('error.connectFirst')); return; }
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
      if (!name) throw new LocalizedError('error.button', { label });
      const rpc = api.value!;
      await rpc.request('buttons', 'pulse', [name, 180]);
      notify(msg('msg.pulsed', { label: label.toUpperCase() }));
    });
  }
  async function releaseButtons(): Promise<void> {
    await action('release', async () => { await api.value!.request('buttons', 'write_reset'); notify(msg('msg.buttonsReleased')); });
  }
  async function insertCoin(): Promise<void> {
    await action('coin', async () => {
      await api.value!.request('coin', 'insert', [1]);
      notify(msg('msg.coin'));
      try { const data = await api.value!.request('coin', 'get'); if (typeof data[0] === 'number') coinQueue.value = data[0]; } catch { /* Insertion already succeeded; never replay it. */ }
    });
  }
  async function insertCard(unit: number, number: string): Promise<void> {
    await action('card', async () => { await api.value!.request('card', 'insert', [unit, number]); notify(msg('msg.cardInserted', { n: unit + 1 })); });
  }
  async function getMachineCards(): Promise<unknown[]> {
    if (!connected.value || !api.value) throw new LocalizedError('error.connectFirst');
    const epoch = generation;
    const data = await api.value.request('card', 'get_cards');
    if (epoch !== generation) throw new LocalizedError('error.cardsChanged');
    return data;
  }
  async function keypad(unit: number, text: string): Promise<void> {
    await action('keypad', async () => { await api.value!.request('keypads', 'write', [unit, text]); notify(msg('msg.keypad')); });
  }

  async function capture(): Promise<void> {
    if (!connected.value || capturing.value) return;
    if (demo.value) { notify(msg('msg.gridOnly')); return; }
    if (!activeProfile) return;
    const epoch = generation;
    capturing.value = true; error.value = '';
    const next = new SpiceClient(15000); captureClient = next;
    try {
      await next.connect(activeProfile.endpoint, activeProfile.password);
      const screens = await next.request('capture', 'get_screens');
      const screen = screens.find(v => Number.isInteger(v) && Number(v) >= 0);
      if (screen === undefined) throw new LocalizedError('error.noCapture');
      const data = await next.request('capture', 'get_jpg', [screen, 65, 2]);
      if (typeof data[3] !== 'string' || !/^[A-Za-z0-9+/=\r\n]+$/.test(data[3])) throw new LocalizedError('error.jpeg');
      if (epoch === generation) {
        image.value = 'data:image/jpeg;base64,' + data[3];
        imageTime.value = new Date().toLocaleTimeString(locale.value, { hour12: false });
        notify(msg('msg.captureUpdated'));
      }
    } catch (value) { if (epoch === generation) report(value instanceof ApiError ? new LocalizedError('error.capture', { detail: value.message }) : value); }
    finally { next.disconnect(); if (captureClient === next) captureClient = undefined; capturing.value = false; }
  }

  return { status, demo, message, error, busy, latency, game, version, buttons, coinQueue, resize, resizeError,
    applying, unsaved, image, imageTime, capturing, configuredPassword, connected,
    connect, startDemo, disconnect, notify, report, updateResize, selectScene, readResize, saveResize,
    machineControl, pulse, exactButton, releaseButtons, insertCoin, insertCard, getMachineCards, keypad, capture,
    currentScene: computed(() => resize.value?.scenes[(resize.value?.scene ?? 1) - 1] ?? defaultScene()) };
}
