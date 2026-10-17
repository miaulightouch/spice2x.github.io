import { locale, msg, textRef, errorText, renderText, LocalizedError } from './i18n';
import { computed, ref, shallowRef, toRaw } from 'vue';
import { ApiError, SpiceClient, asError, isRecord } from './api/client';
import type { ConnectionStatus, Rpc } from './api/client';
import { DemoApi } from './api/demo';
import { ResizeQueue, defaultScene, parseResize } from './api/resize';
import type { ResizePatch, ResizeState } from './api/resize';
import { readCaptureFrame as fetchCaptureFrame, readCaptureScreens as fetchCaptureScreens, parseCaptureStreams, mjpegUrl } from './api/capture';
import type { CaptureFrame, CaptureStreams } from './api/capture';
import type { TextValue } from './i18n';
import type { Profile } from './storage';
import { readLog as fetchLog, downloadLog as fetchLogFile } from './api/log';
import {
  parseCardManager, parseGames, parseIoAnalogs, parseIoButtons, parseIoLights, parseOptions, parsePatches, normalizePatchUrl, parseKeypadCapabilities, parseOptionChoices,
} from './api/features';
import type {
  CardManagerState, GameList, IoAnalog, IoButton, IoLight, OptionsState, PatchState, KeypadCapabilities, OptionElement,
} from './api/features';
import { validateIoPulse } from './api/io';

export type MemoryState = {
  mem_total: number;
  mem_total_used: number;
  mem_used: number;
  vmem_total: number;
  vmem_total_used: number;
  vmem_used: number;
};

function parseMemory(data: unknown[]): MemoryState {
  const value = data[0];
  if (!isRecord(value)) throw new LocalizedError('error.unexpected');
  const read = (name: keyof MemoryState): number => {
    const number = value[name];
    if (typeof number !== 'number' || !Number.isFinite(number) || number < 0) throw new LocalizedError('error.unexpected');
    return number;
  };
  return {
    mem_total: read('mem_total'), mem_total_used: read('mem_total_used'), mem_used: read('mem_used'),
    vmem_total: read('vmem_total'), vmem_total_used: read('vmem_total_used'), vmem_used: read('vmem_used'),
  };
}

export function useController(createDemoApi: () => Rpc = () => new DemoApi()) {
  const status = ref<ConnectionStatus>('disconnected');
  const demo = ref(false);
  const message = textRef();
  const error = textRef();
  const busy = ref('');
  const latency = ref(0);
  const game = ref('');
  const version = ref('');
  const memory = ref<MemoryState>();
  const buttons = ref<string[]>([]);
  const coinQueue = ref<number>();
  const resize = ref<ResizeState>();
  const resizeError = textRef();
  const applying = ref(false);
  const unsaved = ref(false);
  const image = ref('');
  const imageTime = ref('');
  const capturing = ref(false);
  const captureScreens = ref<number[]>([]);
  const selectedCaptureScreen = ref<number>();
  const captureFrame = ref<CaptureFrame>();
  const captureError = textRef();
  const streams = ref<CaptureStreams>();
  const streamUrl = ref('');
  const keypadCapabilities = ref<KeypadCapabilities>();
  const avsInfo = ref<Record<string, string>>({});
  const launcherInfo = ref<Record<string, string>>({});
  const configuredPassword = ref(false);
  const logText = ref('');
  const logSize = ref(0);
  const games = ref<GameList>();
  const options = ref<OptionsState>();
  const patches = ref<PatchState>();
  const cardManager = ref<CardManagerState>();
  const ioButtons = ref<IoButton[]>([]);
  const ioAnalogs = ref<IoAnalog[]>([]);
  const ioLights = ref<IoLight[]>([]);
  const featureErrors = ref<Record<string, string>>({});
  const connected = computed(() => demo.value || status.value === 'connected');
  const api = shallowRef<Rpc>();
  let client: SpiceClient | undefined;
  let captureClient: SpiceClient | undefined;
  let patchImportClient: SpiceClient | undefined;
  let activeProfile: Profile | undefined;
  let queue: ResizeQueue | undefined;
  let generation = 0;
  let revision = 0;
  let confirmed: ResizeState | undefined;
  let resizeApplyError: Error | undefined;
  let ioPollTimer: ReturnType<typeof setInterval> | undefined;
  let ioPollBusy = false;
  let ioReadBusy = false;
  let ioPollGeneration = 0;
  let logPollTimer: ReturnType<typeof setTimeout> | undefined;
  let logPollGeneration = 0;
  const logPolling = ref(false);
  let featureSequence = 0;
  const latestFeatureRequests = new Map<string, number>();

  function notify(text: TextValue): void { message.value = text; error.value = ''; }
  function report(value: unknown): void { error.value = errorText(value); }
  function disconnect(): void {
    ++generation;
    stopIoPolling();
    stopLogPolling();
    queue?.cancel(); queue = undefined;
    const old = client; client = undefined;
    old?.disconnect(); captureClient?.disconnect(); captureClient = undefined;
    patchImportClient?.disconnect(); patchImportClient = undefined;
    api.value = undefined; status.value = 'disconnected'; demo.value = false;
    resize.value = undefined; resizeError.value = ''; buttons.value = []; confirmed = undefined;
    image.value = ''; imageTime.value = ''; captureScreens.value = []; selectedCaptureScreen.value = undefined;
    captureFrame.value = undefined; captureError.value = ''; capturing.value = false;
    streams.value = undefined; streamUrl.value = ''; keypadCapabilities.value = undefined;
    avsInfo.value = {}; launcherInfo.value = {};
    game.value = ''; version.value = ''; memory.value = undefined;
    configuredPassword.value = false; applying.value = false; unsaved.value = false;
    coinQueue.value = undefined; activeProfile = undefined; resizeApplyError = undefined;
    logText.value = ''; logSize.value = 0;
    games.value = undefined; options.value = undefined; patches.value = undefined; cardManager.value = undefined;
    ioButtons.value = []; ioAnalogs.value = []; ioLights.value = []; featureErrors.value = {};
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
    demo.value = true; api.value = createDemoApi();
    await load(generation);
    notify(msg('msg.demo'));
  }

  async function load(epoch: number): Promise<void> {
    const rpc = api.value;
    if (!rpc) return;
    const results = await Promise.allSettled([
      rpc.request('buttons', 'read'), rpc.request('info', 'avs'),
      rpc.request('info', 'launcher'), rpc.request('resize', 'image_resize_get'), rpc.request('coin', 'get'), rpc.request('info', 'memory'),
    ]);
    if (epoch !== generation) return;
    const [b, a, l, r, c, m] = results;
    if (b?.status === 'fulfilled') buttons.value = b.value.filter(v => Array.isArray(v) && typeof v[0] === 'string').map(v => (v as string[])[0]!);
    if (a?.status === 'fulfilled') acceptAvs(a.value);
    if (l?.status === 'fulfilled') acceptLauncher(l.value);
    if (m?.status === 'fulfilled') {
      try { memory.value = parseMemory(m.value); } catch { /* Older binaries may omit the optional status payload. */ }
    }
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
    await refreshKeypadCapabilities();
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

  async function action(key: string, fn: () => Promise<void>): Promise<boolean> {
    if (busy.value) return false;
    if (!connected.value || !api.value) { report(new LocalizedError('error.connectFirst')); return false; }
    const epoch = generation;
    busy.value = key; error.value = '';
    try { await fn(); return epoch === generation; }
    catch (value) { if (epoch === generation) report(value); return false; }
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

  async function captureScreen(screen?: number, quality = 65, divide = 2): Promise<void> {
    if (demo.value) { notify(msg('msg.gridOnly')); return; }
    if (!connected.value || capturing.value || !activeProfile) return;
    const epoch = generation;
    capturing.value = true; error.value = ''; captureError.value = '';
    const next = new SpiceClient(15000); captureClient = next;
    try {
      await next.connect(activeProfile.endpoint, activeProfile.password);
      let screens: number[] | undefined;
      let selected = screen;
      if (selected === undefined) {
        screens = await fetchCaptureScreens(next);
        selected = selectedCaptureScreen.value;
        if (selected === undefined || !screens.includes(selected)) selected = screens[0];
      }
      if (selected === undefined) throw new LocalizedError('error.noCapture');
      const frame = await fetchCaptureFrame(next, selected, quality, divide);
      if (epoch !== generation) return;
      if (screens) captureScreens.value = screens;
      selectedCaptureScreen.value = selected;
      captureFrame.value = frame;
      image.value = 'data:image/jpeg;base64,' + frame.jpeg;
      imageTime.value = new Date().toLocaleTimeString(locale.value, { hour12: false });
      notify(msg('msg.captureUpdated'));
    } catch (value) {
      if (epoch === generation) {
        const failure = value instanceof ApiError ? new LocalizedError('error.capture', { detail: value.message }) : value;
        captureError.value = errorText(failure); report(failure);
      }
    } finally {
      next.disconnect();
      if (captureClient === next) captureClient = undefined;
      if (epoch === generation) capturing.value = false;
    }
  }

  async function capture(): Promise<void> { await captureScreen(); }

  async function readLog(pollToken?: number): Promise<boolean> {
    return action('log', async () => {
      if (!demo.value && !configuredPassword.value) throw new LocalizedError('error.logPassword');
      const epoch = generation;
      const chunk = await fetchLog(api.value!);
      if (epoch !== generation || (pollToken !== undefined && pollToken !== logPollGeneration)) return;
      logText.value = new TextDecoder().decode(chunk.bytes); logSize.value = chunk.size;
    });
  }

  function stopLogPolling(): void {
    ++logPollGeneration; clearTimeout(logPollTimer); logPollTimer = undefined; logPolling.value = false;
  }

  function startLogPolling(): void {
    stopLogPolling();
    const token = logPollGeneration;
    logPolling.value = true;
    const tick = async (): Promise<void> => {
      if (token !== logPollGeneration || !connected.value) { if (token === logPollGeneration) stopLogPolling(); return; }
      const ok = busy.value ? true : await readLog(token);
      if (token !== logPollGeneration) return;
      if (!ok) { stopLogPolling(); return; }
      logPollTimer = setTimeout(() => { void tick(); }, 1000);
    };
    void tick();
  }

  function acceptAvs(data: unknown[]): void {
    const value = data[0];
    if (!isRecord(value)) return;
    avsInfo.value = Object.fromEntries(['model', 'dest', 'spec', 'rev', 'ext', 'services'].filter(key => typeof value[key] === 'string').map(key => [key, value[key] as string]));
    game.value = avsInfo.value.model ?? '';
  }

  function acceptLauncher(data: unknown[]): void {
    const value = data[0];
    if (!isRecord(value)) return;
    // Launch arguments can contain API passwords; show only public build/time fields.
    launcherInfo.value = Object.fromEntries(['version', 'compile_date', 'compile_time', 'system_time'].filter(key => typeof value[key] === 'string').map(key => [key, value[key] as string]));
    version.value = launcherInfo.value.version ?? '';
  }

  async function refreshKeypadCapabilities(): Promise<void> {
    let request: FeatureRequest | undefined;
    try {
      request = featureRpc('keypads');
      const value = parseKeypadCapabilities(await request.rpc.request('keypads', 'capabilities'));
      if (featureRequestCurrent(request)) keypadCapabilities.value = value;
    } catch { /* Older hosts keep their existing two-reader controls. */ }
  }

  async function insertKeypadCard(reader: number): Promise<void> {
    await action('keypad-insert', async () => {
      if (!keypadCapabilities.value?.insert_card) throw new LocalizedError('error.featureUnavailable');
      await api.value!.request('keypads', 'insert_card', [reader]);
      notify(msg('msg.cardInserted', { n: reader + 1 }));
    });
  }

  async function startStream(quality = 65, fps = 30): Promise<void> {
    await action('stream', async () => {
      if (!activeProfile) throw new LocalizedError('error.demoCapture');
      const request = featureRpc('streams');
      try {
        const value = parseCaptureStreams(await request.rpc.request('capture', 'get_streams'));
        if (!featureRequestCurrent(request)) return;
        streams.value = value;
        const screen = selectedCaptureScreen.value ?? value?.screens[0]?.screen;
        if (!value || screen === undefined) throw new LocalizedError('preview.streamUnavailable');
        streamUrl.value = mjpegUrl(activeProfile.endpoint, value, screen, quality, fps);
        selectedCaptureScreen.value = screen;
      } catch (value) { if (featureRequestCurrent(request)) throw value; }
    });
  }

  function stopStream(): void { latestFeatureRequests.set('streams', ++featureSequence); streamUrl.value = ''; }

  type FeatureRequest = { rpc: Rpc; epoch: number; id: number; key: string };

  async function readStatus(): Promise<void> {
    let request: FeatureRequest | undefined;
    try {
      request = featureRpc('status');
      const { rpc } = request;
      const result = await Promise.allSettled([
        rpc.request('info', 'avs'), rpc.request('info', 'launcher'), rpc.request('info', 'memory'),
      ]);
      if (!featureRequestCurrent(request)) return;
      const [avs, launcher, memoryResult] = result;
      if (avs.status === 'fulfilled') acceptAvs(avs.value);
      if (launcher.status === 'fulfilled') acceptLauncher(launcher.value);
      if (memoryResult.status === 'fulfilled') memory.value = parseMemory(memoryResult.value);
      if (result.every(item => item.status === 'rejected')) throw (result[0] as PromiseRejectedResult).reason;
      featureErrors.value = { ...featureErrors.value, status: '' };
    } catch (value) { featureFailure('status', value, request); }
  }

  function featureRequestCurrent(request: FeatureRequest): boolean {
    return request.epoch === generation && latestFeatureRequests.get(request.key) === request.id;
  }

  function featureFailure(name: string, value: unknown, request?: FeatureRequest): void {
    if (request && !featureRequestCurrent(request)) return;
    featureErrors.value = { ...featureErrors.value, [name]: renderText(errorText(value)) };
  }

  function featureRpc(key: string): FeatureRequest {
    if (!connected.value || !api.value) throw new LocalizedError('error.connectFirst');
    const request = { rpc: api.value, epoch: generation, id: ++featureSequence, key };
    latestFeatureRequests.set(key, request.id);
    return request;
  }

  async function refreshGames(): Promise<GameList | undefined> {
    let request: FeatureRequest | undefined;
    try {
      request = featureRpc('options');
      const { rpc } = request;
      const value = parseGames(await rpc.request('config', 'get_games'));
      if (!featureRequestCurrent(request)) return undefined;
      games.value = value; featureErrors.value = { ...featureErrors.value, options: '' };
      return value;
    } catch (value) { featureFailure('options', value, request); return undefined; }
  }

  async function refreshOptions(gameName = options.value?.game || games.value?.current || game.value): Promise<void> {
    let request: FeatureRequest | undefined;
    try {
      const list = games.value ?? await refreshGames();
      const target = gameName || list?.current;
      if (!target) return;
      request = featureRpc('options');
      const { rpc } = request;
      const value = parseOptions(await rpc.request('config', 'get_options', [target]));
      if (!featureRequestCurrent(request)) return;
      options.value = value; featureErrors.value = { ...featureErrors.value, options: '' };
    } catch (value) { featureFailure('options', value, request); }
  }

  async function saveOptions(changes: Array<{ name: string; value: string }>, expectedRevision?: number): Promise<boolean> {
    return action('options-save', async () => {
      const current = options.value;
      if (!current) throw new LocalizedError('error.featureUnavailable');
      const { rpc, epoch } = featureRpc('options-write');
      const revision = expectedRevision ?? current.revision;
      const fresh = parseOptions(await rpc.request('config', 'set_options', [current.game, revision, changes]));
      if (epoch !== generation) return;
      options.value = fresh; featureErrors.value = { ...featureErrors.value, options: '' };
      notify(msg('msg.optionsSaved'));
    });
  }

  async function getOptionChoices(name: string): Promise<OptionElement[] | undefined> {
    let choices: OptionElement[] | undefined;
    await action('option-choices', async () => {
      const target = options.value?.game;
      if (!target) throw new LocalizedError('error.featureUnavailable');
      const request = featureRpc('option-choices');
      const picker = options.value?.options.find(option => option.name === name)?.picker;
      const value = parseOptionChoices(await request.rpc.request('config', 'get_option_choices', [target, name]), name, picker);
      if (featureRequestCurrent(request) && target === options.value?.game) choices = value;
    });
    return choices;
  }

  async function refreshPatches(): Promise<void> {
    let request: FeatureRequest | undefined;
    try {
      request = featureRpc('patches');
      const { rpc } = request;
      const value = parsePatches(await rpc.request('patches', 'get'));
      if (!featureRequestCurrent(request)) return;
      patches.value = value; featureErrors.value = { ...featureErrors.value, patches: '' };
    } catch (value) { featureFailure('patches', value, request); }
  }

  async function setPatch(id: string, change: { enabled?: boolean; choice?: string; value?: number }): Promise<void> {
    await action('patch', async () => {
      const { rpc, epoch } = featureRpc('patch-write');
      const value = parsePatches(await rpc.request('patches', 'set', [id, change]));
      if (epoch !== generation) return;
      patches.value = value; featureErrors.value = { ...featureErrors.value, patches: '' };
    });
  }

  async function setPatchAutoApply(enabled: boolean): Promise<void> {
    await action('patch-auto', async () => {
      const { rpc, epoch } = featureRpc('patch-auto-write');
      const value = parsePatches(await rpc.request('patches', 'set_auto_apply', [enabled]));
      if (epoch !== generation) return;
      patches.value = value;
    });
  }

  async function patchOperation(fn: 'set_group' | 'disable_all' | 'remove_recent_url' | 'clear_recent_urls', params: unknown[] = []): Promise<void> {
    await action('patch-' + fn, async () => {
      const request = featureRpc('patch-operation');
      try {
        const value = parsePatches(await request.rpc.request('patches', fn, params));
        if (featureRequestCurrent(request)) patches.value = value;
      } catch (value) {
        if (featureRequestCurrent(request) && value instanceof ApiError) await refreshPatches();
        throw value;
      }
    });
  }

  async function savePatches(): Promise<void> {
    await action('patch-save', async () => {
      const { rpc, epoch } = featureRpc('patch-save');
      const value = parsePatches(await rpc.request('patches', 'save'));
      if (epoch !== generation) return;
      patches.value = value; notify(msg('msg.patchesSaved'));
    });
  }

  async function reloadPatches(): Promise<void> {
    await action('patch-reload', async () => {
      const { rpc, epoch } = featureRpc('patch-reload');
      const value = parsePatches(await rpc.request('patches', 'reload'));
      if (epoch !== generation) return;
      patches.value = value; notify(msg('msg.patchesReloaded'));
    });
  }

  async function importPatches(url: string): Promise<boolean> {
    return action('patch-import', async () => {
      const source = normalizePatchUrl(url);
      const request = featureRpc('patch-import');
      if (!patches.value?.can_import) throw new LocalizedError('error.patchImportMode');
      if (!demo.value && !configuredPassword.value) throw new LocalizedError('patches.passwordHint');
      const next = demo.value ? undefined : new SpiceClient(120000);
      let sent = false;
      try {
        if (next) {
          const profile = activeProfile!;
          patchImportClient = next;
          await next.connect(profile.endpoint, profile.password);
        }
        if (!featureRequestCurrent(request)) return;
        sent = true;
        const value = parsePatches(await (next ?? request.rpc).request('patches', 'import_url', [source]));
        if (!featureRequestCurrent(request)) return;
        patches.value = value;
        notify(msg('msg.patchesImported'));
      } catch (value) {
        if (!featureRequestCurrent(request)) return;
        if (value instanceof ApiError) {
          await refreshPatches();
          throw value;
        }
        if (sent) throw new LocalizedError('error.patchImportUnknown', { detail: renderText(errorText(value)) });
        throw value;
      } finally {
        next?.disconnect();
        if (patchImportClient === next) patchImportClient = undefined;
      }
    });
  }

  async function refreshCardManager(): Promise<void> {
    let request: FeatureRequest | undefined;
    try {
      request = featureRpc('manager');
      const { rpc } = request;
      const value = parseCardManager(await rpc.request('card', 'manager_get'));
      if (!featureRequestCurrent(request)) return;
      cardManager.value = value; featureErrors.value = { ...featureErrors.value, manager: '' };
    } catch (value) { featureFailure('manager', value, request); }
  }

  async function saveCardManager(cards: CardManagerState['cards'], expectedRevision?: number): Promise<boolean> {
    return action('manager-save', async () => {
      const current = cardManager.value;
      if (!current) throw new LocalizedError('error.featureUnavailable');
      const { rpc, epoch } = featureRpc('manager-save');
      const revision = expectedRevision ?? current.revision;
      const value = parseCardManager(await rpc.request('card', 'manager_save', [revision, cards]));
      if (epoch !== generation) return;
      cardManager.value = value; notify(msg('msg.cardManagerSaved'));
    });
  }

  async function setManagerActive(reader: 0 | 1, id: string): Promise<boolean> {
    return action('manager-active', async () => {
      const { rpc, epoch } = featureRpc('manager-active');
      const value = parseCardManager(await rpc.request('card', 'manager_set_active', [reader, id]));
      if (epoch !== generation) return;
      cardManager.value = value;
    });
  }

  async function loadAndInsertCard(reader: number, id: string): Promise<void> {
    await action('manager-insert', async () => {
      const request = featureRpc('manager-insert');
      const value = parseCardManager(await request.rpc.request('card', 'manager_set_active', [reader, id]));
      if (!featureRequestCurrent(request)) return;
      cardManager.value = value;
      await request.rpc.request('card', 'insert', [reader, id]);
      if (featureRequestCurrent(request)) notify(msg('msg.cardInserted', { n: reader + 1 }));
    });
  }

  async function readIoRequest(pollToken?: number): Promise<void> {
    if (busy.value || ioReadBusy) return;
    ioReadBusy = true;
    let request: FeatureRequest | undefined;
    try {
      request = featureRpc('io');
      const { rpc } = request;
      const result = await Promise.allSettled([
        rpc.request('buttons', 'read'), rpc.request('analogs', 'read'), rpc.request('lights', 'read'),
      ]);
      if (!featureRequestCurrent(request) || (pollToken !== undefined && pollToken !== ioPollGeneration)) return;
      const [buttonsResult, analogsResult, lightsResult] = result;
      if (buttonsResult.status === 'fulfilled') ioButtons.value = parseIoButtons(buttonsResult.value);
      if (analogsResult.status === 'fulfilled') ioAnalogs.value = parseIoAnalogs(analogsResult.value);
      if (lightsResult.status === 'fulfilled') ioLights.value = parseIoLights(lightsResult.value);
      const rejected = result.find((item): item is PromiseRejectedResult => item.status === 'rejected');
      if (rejected && result.every(item => item.status === 'rejected')) throw rejected.reason;
      featureErrors.value = { ...featureErrors.value, io: '' };
    } catch (value) {
      if (request && !featureRequestCurrent(request)) return;
      if (pollToken !== undefined && pollToken !== ioPollGeneration) return;
      featureFailure('io', value, request);
    } finally { ioReadBusy = false; }
  }

  async function readIo(): Promise<void> { await readIoRequest(); }

  async function testIo(kind: 'button' | 'analog' | 'light', name: string, value = 1, duration = 180): Promise<void> {
    await action('io-test', async () => {
      if (!validateIoPulse(value, duration)) throw new LocalizedError('io.invalid');
      const { rpc } = featureRpc('io-test');
      if (kind === 'button') await rpc.request('buttons', 'pulse', [name, duration]);
      else await rpc.request(kind === 'analog' ? 'analogs' : 'lights', 'pulse', [name, value, duration]);
      notify(msg('msg.ioTested', { name }));
    });
  }

  async function resetIo(kind?: 'button' | 'analog' | 'light', name?: string): Promise<void> {
    await action('io-reset', async () => {
      const { rpc } = featureRpc('io-reset');
      const params = name === undefined ? [] : [[name]];
      if (!kind || kind === 'button') await rpc.request('buttons', 'write_reset', params);
      if (!kind || kind === 'analog') await rpc.request('analogs', 'write_reset', params);
      if (!kind || kind === 'light') await rpc.request('lights', 'write_reset', params);
      notify(msg('msg.ioReset'));
    });
  }

  function stopIoPolling(): void {
    ++ioPollGeneration;
    if (ioPollTimer) clearInterval(ioPollTimer);
    ioPollTimer = undefined; ioPollBusy = false;
  }

  function startIoPolling(interval = 300): void {
    stopIoPolling();
    const pollToken = ioPollGeneration;
    ioPollBusy = true; void readIoRequest(pollToken).finally(() => { ioPollBusy = false; });
    ioPollTimer = setInterval(() => {
      if (!connected.value || ioPollBusy || ioReadBusy || busy.value) return;
      const nextToken = ioPollGeneration;
      ioPollBusy = true; void readIoRequest(nextToken).finally(() => { ioPollBusy = false; });
    }, Math.max(200, Math.min(2000, interval)));
  }
  async function downloadLog(): Promise<Blob | undefined> {
    let file: Blob | undefined;
    await action('log-download', async () => {
      if (!demo.value && !configuredPassword.value) throw new LocalizedError('error.logPassword');
      const epoch = generation;
      file = await fetchLogFile(api.value!, () => epoch === generation);
    });
    return file;
  }

  return { status, demo, message, error, busy, latency, game, version, memory, buttons, coinQueue, resize, resizeError,
    applying, unsaved, image, imageTime, capturing, captureScreens, selectedCaptureScreen, captureFrame, captureError,
    configuredPassword, connected,
    connect, startDemo, disconnect, notify, report, updateResize, selectScene, readResize, saveResize,
    machineControl, pulse, exactButton, releaseButtons, insertCoin, insertCard, getMachineCards, keypad, capture,
    captureScreen, streams, streamUrl, startStream, stopStream,
    keypadCapabilities, refreshKeypadCapabilities, insertKeypadCard, loadAndInsertCard, avsInfo, launcherInfo,
    logText, logSize, readLog, downloadLog, readStatus, logPolling, startLogPolling, stopLogPolling,
    games, options, patches, cardManager, ioButtons, ioAnalogs, ioLights, featureErrors,
    refreshGames, refreshOptions, getOptionChoices, saveOptions, refreshPatches, setPatch, patchOperation, setPatchAutoApply, savePatches, reloadPatches, importPatches,
    refreshCardManager, saveCardManager, setManagerActive, readIo, testIo, resetIo, startIoPolling, stopIoPolling,
    currentScene: computed(() => resize.value?.scenes[(resize.value?.scene ?? 1) - 1] ?? defaultScene()) };
}
