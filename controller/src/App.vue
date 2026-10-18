<script setup lang="ts">
import { t, msg, locale, languages, setLocale, LocalizedError } from './i18n';
import { computed, nextTick, onBeforeUnmount, reactive, ref, watch } from 'vue';
import IoPanel from './components/IoPanel.vue';
import CapturePlayer from './components/CapturePlayer.vue';
import { useController } from './useController';
import { isRecord, normalizeEndpoint } from './api/client';
import { EMBEDDED_PROFILE_ID, embeddedWebSocketEndpoint, isEmbeddedBuild, isEmbeddedProfile } from './mode';
import { defaultScene } from './api/resize';
import type { ResizePatch } from './api/resize';
import { loadCards, loadProfiles, normalizeCard, saveCards, saveProfiles } from './storage';
import type { Locale } from './i18n';
import type { Profile } from './storage';
import { generateCardId, groupPatches } from './api/features';
import type { ManagedCard, OptionField, OptionElement } from './api/features';
import { clearOptionDraft as clearDraftState, clearOptionDraftTouches, isOptionDraftDirty, mergeOptionDraft, optionDraftChanges, prepareOptionGameChange, setOptionDraftValue, validOptionValue } from './api/options';
import type { OptionDraftState } from './api/options';
import { cloneCardManagerCards, mergeCardManagerDraft } from './api/cardManagerDraft';

const c = useController();
const { status, demo, connected, busy, error, message, game, version, latency, memory, resize, resizeError,
  currentScene, applying, unsaved, image, imageTime, capturing, configuredPassword, coinQueue, logText, logSize,
  games, options, patches, cardManager, ioButtons, ioAnalogs, ioLights, featureErrors,
  captureScreens, selectedCaptureScreen, captureFrame, captureError, streamUrl, keypadCapabilities, logPolling, avsInfo, launcherInfo } = c;
const logSearch = ref('');
const logAutoScroll = ref(true);
const logViewer = ref<HTMLElement>();
const logLines = computed(() => logText.value.split('\n').filter(line => !logSearch.value || line.toLowerCase().includes(logSearch.value.toLowerCase())));
watch([logLines, logAutoScroll], async () => { if (logAutoScroll.value) { await nextTick(); logViewer.value?.scrollTo({ top: logViewer.value.scrollHeight }); } });
function logLevel(line: string): string { return /\b(?:fatal|error)\b|\b[FE]:/i.test(line) ? 'log-error' : /\b(?:warning|warn)\b|\bW:/i.test(line) ? 'log-warning' : ''; }
async function copyText(value: string): Promise<void> {
  try { await navigator.clipboard.writeText(value); c.notify(msg('msg.copied')); }
  catch { c.report(new LocalizedError('error.clipboard')); }
}
async function downloadLog(): Promise<void> {
  const file = await c.downloadLog();
  if (!file) return;
  const url = URL.createObjectURL(file);
  const link = document.createElement('a'); link.href = url; link.download = 'log.txt';
  document.body.appendChild(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
const embedded = isEmbeddedBuild;
const storedProfiles = ref(loadProfiles());
const profiles = ref(embedded ? storedProfiles.value.filter(profile => isEmbeddedProfile(profile)) : storedProfiles.value);
const selectedId = ref(embedded ? EMBEDDED_PROFILE_ID : profiles.value[0]?.id ?? '');
const selected = computed(() => profiles.value.find(p => p.id === selectedId.value));
const draft = reactive<Profile>({ id: embedded ? EMBEDDED_PROFILE_ID : newId(), name: '', endpoint: '', password: '', rememberPassword: false, ...selected.value, ...(embedded ? { id: EMBEDDED_PROFILE_ID, endpoint: embeddedWebSocketEndpoint() ?? '' } : {}) });
const tab = ref(embedded ? 'connection' : profiles.value.length ? 'resize' : 'connection');
const buildVersion = typeof import.meta.env === 'undefined' ? '' : import.meta.env.VITE_BUILD_VERSION;
const step = ref(1);
const cards = ref(loadCards());
const cardNumber = ref('');
const cardName = ref('');
const unit = ref(0);
const machineCards = ref<{ index: number; card_id: string }[]>([]);
const optionGame = ref('');
const optionSearch = ref('');
const optionShowHidden = ref(false);
const optionActiveOnly = ref(false);
const optionView = ref('all');
const optionCategory = ref('');
const optionChoices = ref<Record<string, OptionElement[]>>({});
const optionsLoading = ref(false);
const optionDraft = reactive<Record<string, string>>({});
const optionTouched = reactive<Record<string, boolean>>({});
const optionDraftState = reactive<OptionDraftState>({ game: '', revision: undefined, values: optionDraft, touched: optionTouched });
const patchSearch = ref('');
const patchUrl = ref('');
const patchSort = ref('name');
const cardSearch = ref('');
const ioSearch = ref('');
const managerDraft = ref<ManagedCard[]>([]);
const managerActive = ref<[string, string]>(['', '']);
const managerDirty = ref(false);
const managerBaseRevision = ref<number>();
const showGrid = ref(true);
const captureQuality = ref(65);
const captureDivide = ref(2);
const captureFps = ref(30);
const offline = ref(!navigator.onLine);
const modeLabel = computed(() => demo.value ? t('status.demo') : ({ disconnected: t('status.disconnected'), connecting: t('status.connecting'), connected: t('status.connected'), error: t('status.error') })[status.value]);
const scalePercent = (axis: 'x' | 'y') => Math.round(currentScene.value[axis === 'x' ? 'scale_x' : 'scale_y'] * 1000) / 10;
const controlsDisabled = computed(() => !connected.value || !!busy.value || !resize.value);
const optionsLocked = computed(() => optionsLoading.value || !!busy.value);
const featureReady = computed(() => connected.value && (demo.value || configuredPassword.value));
const readers = computed(() => Array.from({ length: keypadCapabilities.value?.keypad_count ?? 2 }, (_, index) => index as 0 | 1));
watch(readers, values => { if (!values.includes(unit.value as 0 | 1)) unit.value = values[0] ?? 0; });
const visibleManagerCards = computed(() => managerDraft.value.map((card, index) => ({ card, index })).filter(({ card }) => !cardSearch.value || [card.name, card.id].some(value => value.toLowerCase().includes(cardSearch.value.toLowerCase()))));
const visibleReadOnlyCards = computed(() => cardManager.value?.read_only?.filter(card => !cardSearch.value || [card.name, card.id, card.source].some(value => value.toLowerCase().includes(cardSearch.value.toLowerCase()))) ?? []);
const visibleCards = computed(() => cards.value.filter(card => !cardSearch.value || [card.name, card.number].some(value => value.toLowerCase().includes(cardSearch.value.toLowerCase()))));
const ioPanels = computed(() => [
  { kind: 'button' as const, title: t('io.buttons'), items: ioButtons.value },
  { kind: 'analog' as const, title: t('io.analogs'), items: ioAnalogs.value },
  { kind: 'light' as const, title: t('io.lights'), items: ioLights.value },
].map(panel => ({ ...panel, items: panel.items.filter(item => item.name.toLowerCase().includes(ioSearch.value.toLowerCase())) })));
const boardStyle = computed(() => ({ transform: resize.value?.enabled ? `translate(${currentScene.value.offset_x}px, ${currentScene.value.offset_y}px) scale(${currentScene.value.scale_x}, ${currentScene.value.scale_y})` : 'none' }));
const position = computed(() => `X ${signed(currentScene.value.offset_x)} px · Y ${signed(currentScene.value.offset_y)} px`);
function signed(value: number): string { return `${value >= 0 ? '+' : ''}${value}`; }
function formatBytes(value: number): string {
  if (!Number.isFinite(value) || value < 0) return '—';
  const units = ['B', 'KiB', 'MiB', 'GiB'];
  let amount = value; let unit = 0;
  while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit++; }
  return `${amount.toFixed(unit ? 1 : 0)} ${units[unit]}`;
}

function persistProfiles(): boolean {
  try { saveProfiles(storedProfiles.value); return true; }
  catch { c.report(new LocalizedError('error.storage')); return false; }
}
function changeProfile(event: Event): void {
  const picker = event.target as HTMLSelectElement;
  if (picker.selectedOptions[0]?.hasAttribute('data-manage')) { tab.value = 'connection'; picker.value = selectedId.value; return; }
  selectedId.value = picker.value;
  const profile = selected.value;
  if (!profile) return;
  c.disconnect(); Object.assign(draft, profile); machineCards.value = [];
  managerDraft.value = []; managerActive.value = ['', '']; managerBaseRevision.value = undefined; managerDirty.value = false;
  clearOptionDraft(); showGrid.value = true; c.notify(msg('msg.selected', { name: profile.name }));
}
function applyProfile(persist = true): boolean {
  try {
    const endpoint = embedded ? embeddedWebSocketEndpoint() : normalizeEndpoint(draft.endpoint);
    if (!endpoint) throw new LocalizedError('error.endpoint');
    if (!draft.name.trim()) throw new LocalizedError('error.name');
    const saved = { ...draft, id: embedded ? EMBEDDED_PROFILE_ID : draft.id, endpoint, name: draft.name.trim() };
    const index = storedProfiles.value.findIndex(p => p.id === saved.id);
    if (index < 0) storedProfiles.value.push(saved); else storedProfiles.value[index] = saved;
    if (embedded) profiles.value = storedProfiles.value.filter(profile => isEmbeddedProfile(profile));
    else profiles.value = storedProfiles.value;
    selectedId.value = saved.id; Object.assign(draft, saved);
    return persist ? persistProfiles() : true;
  } catch (value) { c.report(value); return false; }
}
async function connect(): Promise<void> {
  if (!applyProfile(false)) return;
  const saved = persistProfiles();
  machineCards.value = []; showGrid.value = true;
  await c.connect({ ...draft });
  if (!saved && connected.value) c.report(new LocalizedError('error.profileNotSaved'));
}
function saveProfile(): void {
  if (applyProfile()) { c.disconnect(); c.notify(msg('msg.profileSaved')); }
}
function addProfile(): void {
  c.disconnect();
  selectedId.value = '';
  Object.assign(draft, { id: newId(), name: '', endpoint: '', password: '', rememberPassword: false });
  tab.value = 'connection';
}
function scale(axis: 'x' | 'y', value: number): void {
  if (!Number.isFinite(value)) return;
  const factor = Math.round(Math.max(60, Math.min(250, value)) * 10) / 1000;
  c.updateResize(axis === 'x' ? { scale_x: factor } : { scale_y: factor });
}
function inputScale(axis: 'x' | 'y', event: Event): void { scale(axis, Number((event.target as HTMLInputElement).value)); }
function toggle(key: 'enabled' | 'linear_filter' | 'keep_aspect_ratio', event: Event): void {
  c.updateResize({ [key]: (event.target as HTMLInputElement).checked } as ResizePatch);
}
function move(direction: string): void {
  const s = currentScene.value;
  const clamp = (n: number) => Math.max(-32768, Math.min(32767, n));
  if (direction === 'center') c.updateResize({ offset_x: 0, offset_y: 0 });
  if (direction === 'up') c.updateResize({ offset_y: clamp(s.offset_y - step.value) });
  if (direction === 'down') c.updateResize({ offset_y: clamp(s.offset_y + step.value) });
  if (direction === 'left') c.updateResize({ offset_x: clamp(s.offset_x - step.value) });
  if (direction === 'right') c.updateResize({ offset_x: clamp(s.offset_x + step.value) });
}
function inputOffset(key: 'offset_x' | 'offset_y', event: Event): void {
  const value = Number((event.target as HTMLInputElement).value);
  if (Number.isInteger(value)) c.updateResize({ [key]: Math.max(-32768, Math.min(32767, value)) });
}
function resetScene(): void { c.updateResize(defaultScene()); }
async function capture(): Promise<void> { await c.capture(); if (image.value) showGrid.value = false; }
async function insert(number = cardNumber.value, reader = unit.value): Promise<void> {
  try { await c.insertCard(reader, normalizeCard(number)); } catch (value) { c.report(value); }
}
function saveCard(): void {
  try {
    const number = normalizeCard(cardNumber.value);
    const existing = cards.value.find(v => v.number === number);
    if (existing) existing.name = cardName.value.trim() || existing.name;
    else cards.value.push({ id: newId(), name: cardName.value.trim() || t('msg.defaultCard', { n: cards.value.length + 1 }), number });
    saveCards(cards.value); c.notify(msg('msg.cardSaved'));
  } catch (value) { c.report(value); }
}
function removeCard(id: string): void {
  cards.value = cards.value.filter(v => v.id !== id);
  try { saveCards(cards.value); } catch (value) { c.report(value); }
}
async function readCards(): Promise<void> {
  try {
    const data = await c.getMachineCards();
    machineCards.value = data.filter(isRecord).filter(v => typeof v.card_id === 'string' && Number.isInteger(v.index)) as { index: number; card_id: string }[];
    c.notify(msg(machineCards.value.length ? 'msg.cardsRead' : 'msg.noCards'));
  } catch (value) { c.report(value); }
}
const optionDirty = computed(() => isOptionDraftDirty(optionDraftState));
function clearOptionDraft(): void {
  clearDraftState(optionDraftState); optionGame.value = '';
}
function syncOptionDraft(state = options.value): void {
  if (!mergeOptionDraft(optionDraftState, state)) return;
  if (!state) { optionGame.value = ''; return; }
  optionGame.value = state.game;
}
watch(options, state => syncOptionDraft(state));
watch(() => options.value?.game, () => { optionChoices.value = {}; });
watch(patches, (state, previous) => { if (!state) patchUrl.value = ''; else if (!previous) patchUrl.value = state.import_url; });
watch(cardManager, state => {
  const next = mergeCardManagerDraft({ cards: managerDraft.value, active: managerActive.value, revision: managerBaseRevision.value, dirty: managerDirty.value }, state);
  managerDraft.value = next.cards; managerActive.value = next.active; managerBaseRevision.value = next.revision; managerDirty.value = next.dirty;
}, { deep: true });
watch(managerActive, active => {
  const id = active[unit.value];
  if (id) cardNumber.value = id;
}, { deep: true });
watch(unit, reader => {
  const id = managerActive.value[reader];
  if (id) cardNumber.value = id;
});
const optionGroups = computed(() => {
  const groups = new Map<string, OptionField[]>();
  for (const option of options.value?.options ?? []) {
    if (!optionShowHidden.value && option.hidden && !option.has_value) continue;
    if (optionActiveOnly.value && !(optionTouched[option.name] ? !!optionDraft[option.name] : option.has_value)) continue;
    const category = optionView.value === 'quick' ? option.quick_setting_category : option.category;
    if (!category || (optionCategory.value && category !== optionCategory.value)) continue;
    const query = optionSearch.value.trim().toLowerCase();
    if (query && ![option.title, option.name, option.category, option.description].some(text => text.toLowerCase().includes(query))) continue;
    const list = groups.get(category) ?? []; list.push(option); groups.set(category, list);
  }
  const order = options.value?.quick_categories ?? [];
  return [...groups.entries()].sort(([a], [b]) => optionView.value === 'quick' ? order.indexOf(a) - order.indexOf(b) : 0);
});
const optionCategories = computed(() => [...new Set((options.value?.options ?? []).map(option => optionView.value === 'quick' ? option.quick_setting_category : option.category).filter((value): value is string => !!value))]);
watch(optionView, () => { optionCategory.value = ''; });
const invalidOptions = computed(() => (options.value?.options ?? []).filter(option => optionTouched[option.name] && !validOptionValue(option, optionDraft[option.name] ?? '')));
const patchGroups = computed(() => groupPatches(patches.value, patchSearch.value, patchSort.value));
async function openOptions(): Promise<void> {
  tab.value = 'settings';
  if (!featureReady.value || optionsLoading.value) return;
  optionsLoading.value = true;
  try {
    await c.refreshGames();
    if (!featureReady.value) return;
    await c.refreshOptions(optionGame.value || undefined);
    syncOptionDraft();
  } finally { optionsLoading.value = false; }
}
async function reloadOptions(): Promise<void> {
  if (!featureReady.value || optionsLoading.value) return;
  const targetGame = optionDraftState.game || optionGame.value || options.value?.game || games.value?.current;
  optionsLoading.value = true;
  try {
    if (optionDirty.value) {
      if (!window.confirm(t('options.discardConfirm'))) return;
      clearOptionDraft();
    }
    if (!featureReady.value) return;
    await c.refreshOptions(targetGame);
    syncOptionDraft();
  } finally { optionsLoading.value = false; }
}
async function selectOptionGame(): Promise<void> {
  if (optionsLoading.value) {
    optionGame.value = optionDraftState.game || options.value?.game || '';
    return;
  }
  const requestedGame = optionGame.value;
  const stateGame = optionDraftState.game || options.value?.game || '';
  if (!optionDraftState.game && stateGame) optionDraftState.game = stateGame;
  const change = prepareOptionGameChange(optionDraftState, requestedGame, () => window.confirm(t('options.discardConfirm')));
  if (!change.accepted) { optionGame.value = change.game; return; }
  optionsLoading.value = true;
  try {
    optionGame.value = change.game;
    await c.refreshOptions(change.game); syncOptionDraft();
    if (options.value?.game !== change.game) optionGame.value = options.value?.game ?? stateGame;
  } finally { optionsLoading.value = false; }
}
async function saveOptionDraft(): Promise<void> {
  if (optionsLocked.value || invalidOptions.value.length || !options.value || optionDraftState.game !== options.value.game || optionGame.value !== options.value.game) return;
  const changes = optionDraftChanges(optionDraftState, options.value.options, options.value.game);
  if (changes.length && await c.saveOptions(changes, optionDraftState.revision)) {
    clearOptionDraftTouches(optionDraftState, changes);
    syncOptionDraft();
  }
}
function markOption(name: string): void { if (!optionsLocked.value) optionTouched[name] = true; }
function setOptionValue(name: string, event: Event): void {
  if (optionsLocked.value) return;
  setOptionDraftValue(optionDraftState, name, (event.target as HTMLInputElement).value);
}
function setOptionBoolean(name: string, event: Event): void {
  if (optionsLocked.value) return;
  optionDraft[name] = (event.target as HTMLInputElement).checked ? '/ENABLED' : '';
  markOption(name);
}
function optionInputType(option: OptionField): string { return option.sensitive ? 'password' : 'text'; }
function resetOption(option: OptionField): void { setOptionDraftValue(optionDraftState, option.name, ''); }
async function loadChoices(option: OptionField): Promise<void> {
  const choices = await c.getOptionChoices(option.name);
  if (choices) optionChoices.value = { ...optionChoices.value, [option.name]: choices };
}
function cpuChecked(option: OptionField, value: string): boolean {
  const mask = optionDraft[option.name] || '';
  return validOptionValue(option, mask) && mask !== '' && (BigInt(/^0x/i.test(mask) ? mask : '0x' + mask) & BigInt(value)) !== 0n;
}
function toggleCpu(option: OptionField, value: string, event: Event): void {
  const mask = optionDraft[option.name] || '';
  const current = validOptionValue(option, mask) && mask ? BigInt(/^0x/i.test(mask) ? mask : '0x' + mask) : 0n;
  const bit = BigInt(value);
  const next = (event.target as HTMLInputElement).checked ? current | bit : current & ~bit;
  setOptionDraftValue(optionDraftState, option.name, next ? '0x' + next.toString(16).toUpperCase() : '');
}
function optionColor(card: ManagedCard): string { return '#' + card.color.map(value => Math.round(Math.max(0, Math.min(1, value)) * 255).toString(16).padStart(2, '0')).join(''); }
function setManagerColor(card: ManagedCard, event: Event): void {
  const value = (event.target as HTMLInputElement).value.slice(1);
  if (!/^[0-9a-f]{6}$/i.test(value)) return;
  card.color = [parseInt(value.slice(0, 2), 16) / 255, parseInt(value.slice(2, 4), 16) / 255, parseInt(value.slice(4), 16) / 255]; managerDirty.value = true;
}
function randomizeCardColor(card: ManagedCard): void { card.color = [Math.random(), Math.random(), Math.random()]; managerDirty.value = true; }
function addManagerCard(): void {
  let id = generateCardId();
  while (managerDraft.value.some(card => card.id === id)) id = generateCardId();
  managerDraft.value.push({ name: t('cards.newCard'), id, color: [0.08, 0.47, 0.39] }); managerDirty.value = true;
}
function deleteManagerCard(index: number): void { managerDraft.value.splice(index, 1); managerDirty.value = true; }
function moveManagerCard(index: number, direction: -1 | 1): void {
  const target = index + direction; if (target < 0 || target >= managerDraft.value.length) return;
  const card = managerDraft.value.splice(index, 1)[0]!; managerDraft.value.splice(target, 0, card); managerDirty.value = true;
}
async function saveManager(): Promise<void> {
  try {
    const cards = cloneCardManagerCards(managerDraft.value).map(card => ({ ...card, id: normalizeCard(card.id) }));
    const revision = managerBaseRevision.value;
    if (revision === undefined) return;
    if (await c.saveCardManager(cards, revision)) {
      managerDirty.value = false;
      if (cardManager.value) {
        const next = mergeCardManagerDraft({ cards: managerDraft.value, active: managerActive.value, revision: managerBaseRevision.value, dirty: false }, cardManager.value);
        managerDraft.value = next.cards; managerActive.value = next.active; managerBaseRevision.value = next.revision;
      }
    }
  } catch (value) { c.report(value); }
}
async function loadManager(): Promise<void> {
  if (managerDirty.value && !window.confirm(t('cards.managerDiscardConfirm'))) return;
  managerDraft.value = []; managerActive.value = ['', '']; managerBaseRevision.value = undefined; managerDirty.value = false;
  await c.refreshCardManager();
}
async function setActiveReader(reader: 0 | 1, event: Event): Promise<void> {
  const id = (event.target as HTMLSelectElement).value;
  if (await c.setManagerActive(reader, id)) {
    managerActive.value[reader] = id;
    if (id) cardNumber.value = id;
  } else (event.target as HTMLSelectElement).value = managerActive.value[reader];
}
function setPatchInteger(id: string, event: Event): void {
  const patch = patches.value?.patches.find(item => item.id === id);
  if (!patch || patch.status !== 'enabled' || patch.unverified || patch.value === undefined) return;
  const raw = (event.target as HTMLInputElement).value;
  if (!raw) return;
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || (patch.min !== undefined && value < patch.min) || (patch.max !== undefined && value > patch.max)) return;
  void c.setPatch(id, { enabled: true, value });
}
function ensureIoPolling(): void { if (tab.value === 'io') c.startIoPolling(); else c.stopIoPolling(); }
watch(tab, ensureIoPolling);
async function openPatches(): Promise<void> { tab.value = 'patches'; if (!featureReady.value || busy.value) return; await c.refreshPatches(); }
async function importPatchUrl(): Promise<void> {
  if (await c.importPatches(patchUrl.value)) patchUrl.value = patches.value?.import_url ?? patchUrl.value;
}
async function openIo(): Promise<void> { tab.value = 'io'; c.startIoPolling(); }
async function openPreview(): Promise<void> {
  tab.value = 'preview';
  if (connected.value && !demo.value && !captureScreens.value.length) await c.readCaptureScreens();
}
async function refreshCapture(): Promise<void> { await c.captureScreen(selectedCaptureScreen.value, captureQuality.value, captureDivide.value); }
function stopPreview(): void { c.stopStream(); }
async function startStream(): Promise<void> { stopPreview(); await c.startStream(captureQuality.value, captureFps.value); }
function streamFailed(event: Event): void {
  if (!streamUrl.value || (event.currentTarget as HTMLImageElement).currentSrc !== streamUrl.value) return;
  c.stopStream(); c.report(new LocalizedError('preview.streamError'));
}
function changeCaptureScreen(event: Event): void { selectedCaptureScreen.value = Number((event.target as HTMLSelectElement).value); }
watch([tab, connected], () => { if (tab.value !== 'preview' || !connected.value) stopPreview(); if (tab.value !== 'log' || !connected.value) c.stopLogPolling(); });
function selectTab(key: string): void {
  if (key === 'settings') { void openOptions(); return; }
  if (key === 'patches') { void openPatches(); return; }
  if (key === 'io') { void openIo(); return; }
  if (key === 'preview') { void openPreview(); return; }
  tab.value = key;
}
const systemActions = computed(() => [
  { command: 'restart', label: t('action.restart'), detail: t('action.restartDetail') },
  { command: 'exit', label: t('action.exit'), detail: t('action.exitDetail') },
  { command: 'reboot', label: t('action.reboot'), detail: t('action.rebootDetail') },
  { command: 'shutdown', label: t('action.shutdown'), detail: t('action.shutdownDetail') },
] as const);
function confirmSystem(command: 'restart' | 'exit' | 'reboot' | 'shutdown'): void {
  const item = systemActions.value.find(item => item.command === command)!;
  const warning = unsaved.value ? '\n' + t('action.unsaved') : '';
  const machine = embedded ? (draft.name || t('msg.cabinet')) : profiles.value.find(item => item.id === selectedId.value)?.name ?? t('msg.cabinet');
  const prompt = t('action.confirm', { machine, action: item.label, detail: item.detail });
  if (window.confirm((demo.value ? t('action.demoPrefix') : '') + prompt + warning)) void c.machineControl(command);
}
function updateOnline(): void { offline.value = !navigator.onLine; }
function updateVisibility(): void { if (document.hidden) { c.stopIoPolling(); c.stopLogPolling(); stopPreview(); } else ensureIoPolling(); }
function newId(): string { return globalThis.crypto?.randomUUID?.() ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`; }
window.addEventListener('online', updateOnline); window.addEventListener('offline', updateOnline);
document.addEventListener('visibilitychange', updateVisibility);
function onPageHide(): void { c.disconnect(); }
window.addEventListener('pagehide', onPageHide);
onBeforeUnmount(() => {
  stopPreview();
  c.disconnect(); window.removeEventListener('online', updateOnline); window.removeEventListener('offline', updateOnline);
  document.removeEventListener('visibilitychange', updateVisibility);
  window.removeEventListener('pagehide', onPageHide);
});
</script>

<template>
  <div class="controller-app">
    <header class="app-header">
      <a class="brand" href="#" @click.prevent="tab = embedded ? 'connection' : profiles.length ? 'resize' : 'connection'" :aria-label="t('app.home')">
        <span class="brand-mark" aria-hidden="true"><i v-for="n in 4" :key="n"></i></span>
        <strong>{{ t('app.title') }}</strong>
      </a>
      <div class="machine-controls">
        <button v-if="embedded" class="quiet compact" type="button" @click="tab = 'connection'">{{ draft.name || t('connection.settings') }}</button>
        <template v-else>
          <label class="sr-only" for="machine">{{ t('connection.select') }}</label>
          <select id="machine" :value="selectedId" :disabled="!profiles.length || !!busy || status === 'connecting'" @change="changeProfile"><option v-if="!selected" value="" disabled>{{ t(profiles.length ? 'connection.newProfile' : 'connection.noProfiles') }}</option><option v-for="p in profiles" :key="p.id" :value="p.id">{{ p.name }}</option><option v-if="profiles.length" value="manage" data-manage>{{ t('connection.manage') }}</option></select>
        </template>
        <button v-if="connected || status === 'connecting'" class="quiet compact" :disabled="!!busy" @click="c.disconnect(); c.notify(msg('msg.disconnected'))">{{ t('connection.disconnect') }}</button>
        <button v-else-if="selected" class="primary compact" :disabled="!!busy" @click="connect">{{ t('connection.connect') }}</button>
        <button v-else-if="!embedded" class="primary compact" @click="tab = 'connection'">{{ t('connection.manage') }}</button>
        <span class="connection-chip" :data-status="demo ? 'demo' : status"><i aria-hidden="true"></i>{{ modeLabel }}</span>
      </div>
      <span class="header-spacer" aria-hidden="true"></span>
      <label class="language-control"><span aria-hidden="true">&#25991;/A <small>▾</small></span><span class="sr-only">{{ t('app.language') }}</span><select :value="locale" @change="setLocale(($event.target as HTMLSelectElement).value as Locale)"><option v-for="language in languages" :key="language.code" :value="language.code" :lang="language.code">{{ language.label }}</option></select></label>
    </header>
    <div v-if="demo" class="demo-strip">{{ t('connection.demoStrip') }}<button class="text-button" @click="c.disconnect(); tab = 'connection'">{{ t('connection.connect') }}</button></div>
    <div v-if="offline" class="offline-strip">{{ t('connection.offline') }}</div>
    <nav class="main-nav" :aria-label="t('nav.label')">
      <button v-for="item in [{ key: 'io', label: t('nav.io'), symbol: '◌' }, { key: 'settings', label: t('nav.settings'), symbol: '☷' }, { key: 'keypad', label: t('nav.keypad'), symbol: '⌨' }, { key: 'cards', label: t('nav.cards'), symbol: '▤' }, { key: 'log', label: t('nav.log'), symbol: '▥' }, { key: 'control', label: t('nav.control'), symbol: '◉' }, { key: 'patches', label: t('nav.patches'), symbol: '⚙' }, { key: 'resize', label: t('nav.resize'), symbol: '▦' }, { key: 'preview', label: t('nav.screen'), symbol: '▣' }, { key: 'status', label: t('nav.status'), symbol: '⌁' }]" :key="item.key" :aria-pressed="tab === item.key" @click="selectTab(item.key)"><span aria-hidden="true">{{ item.symbol }}</span>{{ item.label }}</button>
    </nav>
    <main class="main-content">
      <section v-show="tab === 'settings'" aria-labelledby="options-title">
        <div class="page-title"><h1 id="options-title">{{ t('options.title') }}</h1><div class="button-row"><select v-if="games" v-model="optionGame" :aria-label="t('options.game')" :disabled="!featureReady || optionsLocked" @change="selectOptionGame"><option v-for="name in games.games" :key="name" :value="name">{{ name }}</option></select><button class="quiet compact" :disabled="optionsLocked || !featureReady" @click="reloadOptions">{{ t('options.reload') }}</button></div></div>
        <div v-if="featureErrors.options" class="empty-state"><h2>{{ t('options.unavailable') }}</h2><p>{{ featureErrors.options }}</p><button :disabled="optionsLocked || !featureReady" @click="openOptions">{{ t('options.retry') }}</button></div>
        <div v-else-if="!options" class="empty-state"><h2>{{ t('options.loading') }}</h2><p>{{ t('options.hint') }}</p><p v-if="connected && !featureReady" class="muted small">{{ t('options.passwordHint') }}</p><button :disabled="!featureReady || optionsLocked" @click="openOptions">{{ t('options.load') }}</button></div>
        <template v-else>
          <div class="settings-panel feature-toolbar">
            <label class="stacked-field">{{ t('options.search') }}<input v-model="optionSearch" type="search" :disabled="optionsLocked"></label>
            <label class="stacked-field">{{ t('options.view') }}<select v-model="optionView"><option value="all">{{ t('options.all') }}</option><option value="quick" :disabled="!options.options.some(option => option.quick_setting_category)">{{ t('options.quick') }}</option></select></label>
            <label class="stacked-field">{{ t('options.category') }}<select v-model="optionCategory"><option value="">{{ t('options.all') }}</option><option v-for="category in optionCategories" :key="category">{{ category }}</option></select></label>
            <label class="check"><input v-model="optionActiveOnly" type="checkbox">{{ t('options.activeOnly') }}</label>
            <label class="check"><input v-model="optionShowHidden" type="checkbox">{{ t('options.showHidden') }}</label>
          </div>
          <div class="section-toolbar"><span class="muted small">{{ options.restart_required ? t('options.restartHint') : t('options.liveHint') }}</span><button class="quiet compact" :disabled="!featureReady || optionsLocked || optionDirty" @click="confirmSystem('restart')">{{ t('action.restart') }}</button></div>
          <div class="feature-cards">
            <div v-for="[category, fields] in optionGroups" :key="category" class="settings-panel feature-group">
              <h2>{{ category }}</h2>
              <div v-for="option in fields" :key="option.name" class="feature-field">
                <div class="field-header"><label :for="'option-' + option.name">{{ option.title }}</label><button class="quiet compact" :disabled="option.disabled || optionsLocked" @click="resetOption(option)">{{ t('options.reset') }}</button></div>
                <p v-if="option.description" class="muted small">{{ option.description }}</p>
                <label v-if="option.type === 'bool'" class="check"><input :id="'option-' + option.name" type="checkbox" :checked="optionDraft[option.name] !== ''" :disabled="option.disabled || optionsLocked" @change="setOptionBoolean(option.name, $event)">{{ optionDraft[option.name] !== '' ? t('options.on') : t('options.off') }}</label>
                <select v-else-if="option.type === 'enum'" :id="'option-' + option.name" :value="optionDraft[option.name]" :disabled="option.disabled || optionsLocked" @change="setOptionValue(option.name, $event)"><option value="">{{ t('options.default') }}</option><option v-for="element in option.elements" :key="element.value" :value="element.value">{{ element.label || element.value }}</option></select>
                <input v-else :id="'option-' + option.name" :value="optionDraft[option.name]" :type="optionInputType(option)" :inputmode="option.type === 'integer' ? 'numeric' : undefined" maxlength="4096" :aria-invalid="optionTouched[option.name] && !validOptionValue(option, optionDraft[option.name] ?? '')" :autocomplete="option.sensitive ? 'off' : undefined" :disabled="option.disabled || optionsLocked" :placeholder="option.sensitive ? t('options.sensitivePlaceholder') : option.setting_name" :title="'-' + (option.display_name || option.name)" @input="setOptionValue(option.name, $event)">
                <span v-if="optionTouched[option.name] && !validOptionValue(option, optionDraft[option.name] ?? '')" class="error-text">{{ t('options.invalid') }}</span>
                <div v-if="['asio_driver','monitor','cpu_affinity'].includes(option.picker ?? '')" class="option-picker">
                  <button class="quiet compact" :disabled="option.disabled || optionsLocked" @click="loadChoices(option)">{{ t('options.choose') }}</button>
                  <template v-if="optionChoices[option.name]">
                    <div v-if="option.picker === 'cpu_affinity'" class="cpu-choices"><label v-for="choice in optionChoices[option.name]" :key="choice.value" class="check"><input type="checkbox" :checked="cpuChecked(option, choice.value)" :disabled="option.disabled || optionsLocked" @change="toggleCpu(option, choice.value, $event)">{{ choice.label }}</label></div>
                    <select v-else :aria-label="option.title" :value="optionDraft[option.name]" :disabled="option.disabled || optionsLocked" @change="setOptionValue(option.name, $event)"><option value="">{{ t('options.default') }}</option><option v-for="choice in optionChoices[option.name]" :key="choice.value" :value="choice.value">{{ choice.label }}</option></select>
                    <span v-if="!optionChoices[option.name]?.length" class="muted small">{{ t('options.choicesUnavailable') }}</span>
                  </template>
                </div>
                <button v-if="option.picker === 'ea_card'" class="quiet compact" :disabled="option.disabled || optionsLocked" @click="setOptionDraftValue(optionDraftState, option.name, generateCardId())">{{ t('options.generate') }}</button>
                <span v-if="option.picker === 'file_path' || option.picker === 'directory_path'" class="muted small">{{ t('options.pathHint') }}{{ option.file_extension ? ' · ' + option.file_extension : '' }}</span>
                <span v-if="option.sensitive" class="muted small">{{ t('options.sensitive') }}</span><span v-if="option.hidden" class="muted small">{{ t('options.hidden') }}</span>
              </div>
            </div>
          </div>
          <p v-if="!optionGroups.length" class="empty-state">{{ t('options.noMatches') }}</p>
          <div class="panel-actions"><button class="primary" :disabled="optionsLocked || !featureReady || !optionDirty || !!invalidOptions.length" @click="saveOptionDraft">{{ t('options.save') }}</button><span class="muted small">{{ t('options.revision', { n: options.revision }) }}</span></div>
        </template>
      </section>
      <section v-show="tab === 'patches'" aria-labelledby="patches-title">
        <div class="page-title"><h1 id="patches-title">{{ t('patches.title') }}</h1><div class="button-row"><button class="quiet compact" :disabled="!!busy || !featureReady" @click="c.reloadPatches">{{ t('patches.reload') }}</button><button class="primary compact" :disabled="!!busy || !featureReady || !patches?.dirty" @click="c.savePatches()">{{ t('patches.save') }}</button></div></div>
        <div v-if="featureErrors.patches" class="empty-state"><h2>{{ t('patches.unavailable') }}</h2><p>{{ featureErrors.patches }}</p><button @click="openPatches">{{ t('patches.retry') }}</button></div>
        <div v-else-if="!patches" class="empty-state"><h2>{{ t('patches.loading') }}</h2><p v-if="connected && !featureReady" class="muted small">{{ t('patches.passwordHint') }}</p><button :disabled="!featureReady || !!busy" @click="openPatches">{{ t('patches.load') }}</button></div>
        <template v-else>
          <form class="settings-panel patch-import" @submit.prevent="importPatchUrl">
            <div class="feature-toolbar"><label class="stacked-field">{{ t('patches.importUrl') }}<input v-model="patchUrl" type="url" list="patch-sources" placeholder="https://patch-server.example/" maxlength="4096" required spellcheck="false" autocomplete="off" autocapitalize="none" :disabled="!patches.can_import || !featureReady || !!busy"></label><datalist id="patch-sources"><option v-for="url in patches.recent_urls" :key="url" :value="url"></option></datalist><button type="submit" :disabled="!patches.can_import || !featureReady || !!busy || !patchUrl.trim()">{{ t(busy === 'patch-import' ? 'patches.importing' : 'patches.import') }}</button></div>
            <p class="muted small">{{ t(!patches.standalone ? 'patches.importMode' : !patches.can_import ? 'patches.importUnavailable' : 'patches.importHint') }}</p>
          </form>
          <div v-if="patches.recent_urls.length" class="settings-panel patch-history">
            <div class="section-toolbar"><h2>{{ t('patches.recent') }}</h2><button class="quiet compact" :disabled="!patches.standalone || !featureReady || !!busy" @click="c.patchOperation('clear_recent_urls')">{{ t('patches.clearSources') }}</button></div>
            <div v-for="url in patches.recent_urls" :key="url" class="source-row"><button class="text-button" :disabled="!!busy" @click="patchUrl = url">{{ url }}</button><button class="quiet compact" :aria-label="t('patches.removeSource') + ': ' + url" :disabled="!patches.standalone || !featureReady || !!busy" @click="c.patchOperation('remove_recent_url', [url])">{{ t('cards.delete') }}</button></div>
          </div>
          <div v-if="patches.import_errors" class="settings-panel import-errors"><div class="section-toolbar"><h2>{{ t('status.error') }}</h2><button class="quiet compact" @click="copyText(patches.import_errors)">{{ t('cards.copy') }}</button></div><p class="caution">{{ patches.import_errors }}</p></div>
          <details v-if="patches.identity" class="settings-panel patch-identity"><summary>{{ t('patches.identity') }}</summary><dl class="status-list"><div><dt>{{ t('patches.gameIdentifier') }}</dt><dd>{{ patches.identity.game_identifier || '—' }}</dd></div><div><dt>{{ t('patches.peIdentifier') }}</dt><dd>{{ patches.identity.pe_identifier || '—' }}</dd></div><div><dt>{{ t('patches.patchFile') }}</dt><dd>{{ patches.identity.patch_file || '—' }}</dd></div></dl><button class="quiet compact" @click="copyText(Object.values(patches.identity).filter(Boolean).join('\n'))">{{ t('cards.copy') }}</button></details>
          <div class="settings-panel feature-toolbar patch-toolbar">
            <label class="stacked-field">{{ t('patches.search') }}<input v-model="patchSearch" type="search"></label>
            <label class="stacked-field">{{ t('patches.sort') }}<select v-model="patchSort"><option value="name">{{ t('patches.sortName') }}</option><option value="status">{{ t('patches.sortStatus') }}</option></select></label>
            <label class="check"><input type="checkbox" :checked="patches.auto_apply" :disabled="!!busy || !featureReady" @change="c.setPatchAutoApply(($event.target as HTMLInputElement).checked)">{{ t('patches.autoApply') }}</label>
            <button class="quiet compact" :disabled="!patches.standalone || !!busy || !featureReady" @click="c.patchOperation('disable_all')">{{ t('patches.disableAll') }}</button>
          </div>
          <div class="feature-cards">
            <details v-for="group in patchGroups" :key="group.key" class="settings-panel patch-group" open>
              <summary>{{ group.metadata?.name || group.id || t('patches.ungrouped') }} <span class="muted small">({{ group.items.length }})</span></summary>
              <label v-if="group.metadata" class="check"><input type="checkbox" :checked="group.checked" :indeterminate="group.mixed" :disabled="group.hasError || !!busy || !featureReady" @change="c.patchOperation('set_group', [group.game_code, group.id, ($event.target as HTMLInputElement).checked])">{{ group.metadata.name }}</label>
              <p v-if="group.metadata?.description" class="muted small">{{ group.metadata.description }}</p><p v-if="group.metadata?.caution" class="caution">{{ group.metadata.caution }}</p>
              <div v-for="patch in group.items" :key="patch.id" class="patch-item">
                <div class="field-header"><label class="check"><input type="checkbox" :checked="patch.status === 'enabled'" :disabled="patch.status === 'error' || !!busy || !featureReady" @change="c.setPatch(patch.id, { enabled: ($event.target as HTMLInputElement).checked })">{{ patch.name }}</label><span v-if="patch.unverified" class="caution">{{ t('patches.unverified') }}</span></div>
                <p v-if="patch.description" class="muted small">{{ patch.description }}</p><p v-if="patch.caution" class="caution">{{ patch.caution }}</p><p v-if="patch.error" class="error-text">{{ patch.error }}</p>
                <select v-if="patch.choices.length" :aria-label="patch.name" :value="patch.selected" :disabled="patch.status !== 'enabled' || patch.unverified || !!busy || !featureReady" @change="c.setPatch(patch.id, { enabled: true, choice: ($event.target as HTMLSelectElement).value })"><option v-for="choice in patch.choices" :key="choice" :value="choice">{{ choice }}</option></select>
                <input v-if="patch.type === 'integer'" :aria-label="patch.name" type="number" :min="patch.min" :max="patch.max" :value="patch.value ?? ''" :disabled="patch.status !== 'enabled' || patch.unverified || patch.value === undefined || !!busy || !featureReady" @change="setPatchInteger(patch.id, $event)">
              </div>
            </details>
          </div>
          <p v-if="!patchGroups.length" class="empty-state">{{ t(patches.patches.length ? 'patches.noMatches' : 'patches.empty') }}</p>
        </template>
      </section>
      <section v-show="tab === 'io'" aria-labelledby="io-title">
        <div class="page-title"><h1 id="io-title">{{ t('io.title') }}</h1><div class="button-row"><button class="quiet compact" :disabled="!!busy || !connected" @click="c.readIo">{{ t('io.refresh') }}</button><button class="quiet compact" :disabled="!!busy || !connected" @click="c.resetIo()">{{ t('io.reset') }}</button></div></div>
        <div v-if="featureErrors.io" class="empty-state"><h2>{{ t('io.unavailable') }}</h2><p>{{ featureErrors.io }}</p><button @click="c.readIo">{{ t('io.retry') }}</button></div>
        <template v-else>
          <div class="settings-panel feature-toolbar"><label class="stacked-field">{{ t('io.search') }}<input v-model="ioSearch" type="search"></label></div>
          <div class="io-grid"><IoPanel v-for="panel in ioPanels" :key="panel.kind" :kind="panel.kind" :title="panel.title" :items="panel.items" :disabled="!connected || !!busy" @test="(name, value, duration) => c.testIo(panel.kind, name, value, duration)" @reset="name => c.resetIo(panel.kind, name)" /></div>
        </template>
      </section>
      <section v-show="tab === 'resize'" aria-labelledby="screen-title">
        <div class="page-title"><h1 id="screen-title">{{ t('screen.title') }}</h1><span class="muted small" v-if="resize">{{ applying ? t('screen.updating') : !resize.enabled ? t('screen.disabled') : unsaved ? t('screen.unsaved') : t('screen.saved') }}</span></div>
        <div v-if="!connected" class="empty-state"><h2>{{ t('screen.welcome') }}</h2><p>{{ t('screen.connectHint') }}</p><div class="button-row"><button class="primary" @click="tab = 'connection'">{{ t('screen.setup') }}</button><button @click="c.startDemo()">{{ t('screen.tryDemo') }}</button></div></div>
        <div v-else-if="resizeError" class="empty-state"><h2>{{ t('screen.unavailable') }}</h2><p>{{ resizeError }}</p><p class="small">{{ t('screen.apiHint') }}</p><div class="button-row"><button @click="c.readResize()" :disabled="!!busy">{{ t('screen.read') }}</button><a class="button" href="https://github.com/miaulightouch/spice2x.github.io/releases" target="_blank" rel="noopener noreferrer">{{ t('screen.download') }}</a></div></div>
        <div v-else-if="resize" class="calibration-layout">
          <div class="settings-panel calibration-settings">
            <div class="section-toolbar"><h2>{{ t('screen.scenes') }}</h2><label class="check"><input type="checkbox" :checked="resize.enabled" :disabled="controlsDisabled" @change="toggle('enabled', $event)">{{ t('screen.enable') }}</label></div>
            <div v-if="!resize.enabled" class="resize-inactive" role="status"><p>{{ t('screen.inactive') }}</p></div>
            <div class="scene-picker" role="group" :aria-label="t('screen.scenes')"><button v-for="n in 4" :key="n" :aria-pressed="resize.scene === n" :disabled="controlsDisabled" @click="c.selectScene(n)">{{ t('screen.scene', { n }) }}</button></div>
            <div v-for="axis in (['x', 'y'] as const)" :key="axis" class="scale-field">
              <div class="field-header"><label :for="'scale-' + axis">{{ t(axis === 'x' ? 'screen.horizontal' : 'screen.vertical') }}</label><output class="numeric">{{ scalePercent(axis).toFixed(1) }}%</output></div>
              <input :id="'scale-' + axis" type="range" min="60" max="250" step="0.1" :value="scalePercent(axis)" :disabled="controlsDisabled" @input="inputScale(axis, $event)">
              <div class="number-stepper"><button :aria-label="t('screen.decrease', { axis: t(axis === 'x' ? 'screen.horizontal' : 'screen.vertical') })" :disabled="controlsDisabled" @click="scale(axis, scalePercent(axis) - 0.1)">−</button><input type="number" min="60" max="250" step="0.1" :aria-label="t('screen.percent', { axis: t(axis === 'x' ? 'screen.horizontal' : 'screen.vertical') })" :value="scalePercent(axis)" :disabled="controlsDisabled" @change="inputScale(axis, $event)"><button :aria-label="t('screen.increase', { axis: t(axis === 'x' ? 'screen.horizontal' : 'screen.vertical') })" :disabled="controlsDisabled" @click="scale(axis, scalePercent(axis) + 0.1)">＋</button></div>
            </div>
            <div class="resize-options"><label class="check"><input type="checkbox" :checked="currentScene.keep_aspect_ratio" :disabled="controlsDisabled" @change="toggle('keep_aspect_ratio', $event)">{{ t('screen.aspect') }}</label><label class="check"><input type="checkbox" :checked="resize.linear_filter" :disabled="controlsDisabled" @change="toggle('linear_filter', $event)">{{ t('screen.filter') }}</label></div>
            <label class="duplicate-field">{{ t('screen.duplicate') }}<select :value="currentScene.duplicate" :disabled="controlsDisabled" @change="c.updateResize({ duplicate: Number(($event.target as HTMLSelectElement).value) })"><option :value="0">{{ t('screen.none') }}</option><option :value="1">{{ t('screen.copyLeft') }}</option><option :value="2">{{ t('screen.copyRight') }}</option></select></label>
            <div class="panel-actions"><button :disabled="controlsDisabled" @click="resetScene">{{ t('screen.reset') }}</button><button class="primary" :disabled="controlsDisabled || (!demo && !configuredPassword)" @click="c.saveResize()">{{ t(busy === 'save' ? 'screen.saving' : 'screen.save') }}</button></div>
            <p class="muted small" v-if="!demo && !configuredPassword">{{ t('screen.passwordHint') }}</p>
          </div>
          <div class="settings-panel position-panel">
            <div class="section-toolbar"><h2>{{ t('screen.position') }}</h2><label class="inline-field">{{ t('screen.step') }}<select v-model.number="step"><option v-for="n in [1, 5, 10]" :key="n" :value="n">{{ n }} px</option></select></label></div>
            <div class="position-controls"><output class="numeric">{{ position }}</output><div class="dpad" role="group" :aria-label="t('screen.positionLabel')"><button data-move="up" :aria-label="t('screen.up')" :disabled="controlsDisabled" @click="move('up')">↑</button><button data-move="left" :aria-label="t('screen.left')" :disabled="controlsDisabled" @click="move('left')">←</button><button data-move="center" :aria-label="t('screen.center')" :disabled="controlsDisabled" @click="move('center')">○</button><button data-move="right" :aria-label="t('screen.right')" :disabled="controlsDisabled" @click="move('right')">→</button><button data-move="down" :aria-label="t('screen.down')" :disabled="controlsDisabled" @click="move('down')">↓</button></div></div>
            <div class="offset-inputs"><label>{{ t('screen.offsetX') }}<input type="number" min="-32768" max="32767" step="1" :value="currentScene.offset_x" :disabled="controlsDisabled" @change="inputOffset('offset_x', $event)"></label><label>{{ t('screen.offsetY') }}<input type="number" min="-32768" max="32767" step="1" :value="currentScene.offset_y" :disabled="controlsDisabled" @change="inputOffset('offset_y', $event)"></label></div>
            <details class="preview-panel"><summary>{{ t(!showGrid && image ? 'screen.capture' : 'screen.grid') }}</summary>
              <div class="screen-preview" :class="{ 'is-capture': !showGrid && image }"><template v-if="showGrid || !image"><div class="target-frame" aria-hidden="true"></div><div class="calibration-board" :style="boardStyle" role="img" :aria-label="t('screen.gridAlt')"><div v-for="n in 16" :key="n">{{ String(n).padStart(2, '0') }}</div></div><span class="preview-caption">{{ t('screen.previewHint') }}</span></template><img v-else :src="image" :alt="t('screen.capture')"></div>
              <div class="preview-toolbar"><button class="quiet compact" :disabled="capturing || demo || !!streamUrl || busy === 'stream'" @click="capture">{{ t(capturing ? 'screen.capturing' : 'screen.refreshCapture') }}</button><button v-if="image" class="text-button" @click="showGrid = !showGrid">{{ t(showGrid ? 'screen.showCapture' : 'screen.showGrid') }}</button><span class="muted small" v-if="imageTime">{{ imageTime }} · {{ t('screen.still') }}</span></div>
            </details>
            <button class="text-button read-action" :disabled="controlsDisabled" @click="c.readResize()">{{ t('screen.reload') }}</button>
            <details class="usage-notes"><summary>{{ t('app.details') }}</summary><p>{{ t('screen.saveHint') }}</p><p>{{ t('screen.backendHint') }}</p></details>
          </div>
        </div><p v-else class="muted">{{ t('screen.loading') }}</p>
      </section>
      <section v-show="tab === 'control'" aria-labelledby="operate-title">
        <div class="page-title"><h1 id="operate-title">{{ t('operate.title') }}</h1></div>
        <div class="operation-grid"><button class="operation-button" :disabled="!connected || !!busy || !c.exactButton('Service')" @click="c.pulse('Service')"><span>SERVICE</span><small>{{ t('operate.pulse') }}</small></button><button class="operation-button" :disabled="!connected || !!busy || !c.exactButton('Test')" @click="c.pulse('Test')"><span>TEST</span><small>{{ t('operate.pulse') }}</small></button><button class="operation-button" :disabled="!connected || !!busy" @click="c.insertCoin()"><span>{{ t('operate.coin') }}</span><small>{{ t('operate.coinHint') }}</small></button><button class="operation-button" :disabled="!connected || !!busy" @click="c.releaseButtons()"><span>{{ t('operate.release') }}</span><small>{{ t('operate.releaseHint') }}</small></button></div>
        <div class="settings-panel system-actions"><h2>{{ t('operate.power') }}</h2><div class="operation-grid"><button v-for="item in systemActions" :key="item.command" class="operation-button" :disabled="!connected || !!busy || (!demo && !configuredPassword)" @click="confirmSystem(item.command)"><span>{{ item.label }}</span><small>{{ item.detail }}</small></button></div><p v-if="!demo && !configuredPassword" class="muted small">{{ t('operate.passwordHint') }}</p><details class="usage-notes"><summary>{{ t('app.details') }}</summary><p>{{ t('operate.powerHint') }}</p></details></div>
        <p class="muted small" v-if="coinQueue !== undefined">{{ t('operate.queue', { n: coinQueue }) }}</p><p class="muted" v-if="!connected">{{ t('operate.connectHint') }}</p><p class="muted small" v-else-if="!c.exactButton('Service') || !c.exactButton('Test')">{{ t('operate.missingButtons') }}</p>
      </section>
      <section v-show="tab === 'cards'" aria-labelledby="cards-title">
        <div class="page-title"><h1 id="cards-title">{{ t('cards.title') }}</h1><label class="inline-field">{{ t('cards.reader') }}<select v-model.number="unit"><option v-for="reader in readers" :key="reader" :value="reader">P{{ reader + 1 }}</option></select></label></div>
        <div class="settings-panel feature-toolbar"><label class="stacked-field">{{ t('cards.search') }}<input v-model="cardSearch" type="search"></label></div>
        <div class="cards-layout">
          <div>
            <form class="settings-panel" @submit.prevent="insert()"><h2>{{ t('cards.insertTitle') }}</h2>
              <label class="stacked-field">{{ t('cards.number') }}<input v-model="cardNumber" class="card-number" placeholder="E004 0000 0000 0001" maxlength="32" autocapitalize="characters" spellcheck="false" autocomplete="off"></label>
              <button type="button" class="quiet compact" :disabled="!!busy" @click="cardNumber = generateCardId()">{{ t('options.generate') }}</button><label class="stacked-field">{{ t('cards.name') }}<input v-model="cardName" :placeholder="t('cards.nameHint')" maxlength="60" autocomplete="off"></label>
              <div class="panel-actions"><button type="button" @click="saveCard">{{ t('cards.save') }}</button><button type="submit" class="primary" :disabled="!connected || !!busy || !cardNumber.trim() || !readers.includes(unit as 0 | 1)">{{ t('cards.insert', { n: unit + 1 }) }}</button></div>
            </form>
            <div class="settings-panel saved-cards"><h2>{{ t('cards.mine') }}</h2><p class="muted small" v-if="!cards.length">{{ t('cards.localHint') }}</p>
              <div v-for="card in visibleCards" :key="card.id" class="saved-card"><div><strong>{{ card.name }}</strong><code>{{ card.number.match(/.{1,4}/g)?.join(' ') }}</code></div><div class="button-row"><button :disabled="!connected || !!busy || !readers.includes(unit as 0 | 1)" @click="insert(card.number)">{{ t('cards.use') }}</button><button class="quiet compact" @click="copyText(card.number)">{{ t('cards.copy') }}</button><button class="quiet" :aria-label="t('cards.deleteLabel', { name: card.name })" @click="removeCard(card.id)">{{ t('cards.delete') }}</button></div></div>
              <p v-if="cards.length && !visibleCards.length" class="muted small">{{ t('cards.empty') }}</p>
            </div>
            <button class="text-button" :disabled="!connected || (!demo && !configuredPassword)" @click="readCards">{{ t('cards.read') }}</button><div v-for="card in machineCards" :key="card.index" class="machine-card"><span>P{{ card.index + 1 }} · {{ card.card_id }}</span><button class="quiet compact" @click="cardNumber = card.card_id">{{ t('cards.fill') }}</button></div>
          </div>
          <div class="settings-panel manager-panel">
            <div class="section-toolbar"><h2>{{ t('cards.manager') }}</h2><div class="button-row"><button class="quiet compact" :disabled="!!busy || !featureReady" @click="loadManager">{{ t(managerDirty ? 'cards.managerDiscard' : 'cards.managerLoad') }}</button><button class="primary compact" :disabled="!!busy || !featureReady || !managerDirty || managerBaseRevision === undefined" @click="saveManager">{{ t('cards.managerSave') }}</button></div></div>
            <div v-if="featureErrors.manager" class="empty-state"><p>{{ featureErrors.manager }}</p><button :disabled="!featureReady" @click="loadManager">{{ t('cards.managerRetry') }}</button></div>
            <template v-else-if="cardManager">
              <div v-for="reader in readers" :key="reader" class="stacked-field"><label>{{ t('cards.activeReader', { n: reader + 1 }) }}<select :value="managerActive[reader]" :disabled="!!busy || !featureReady || managerDirty" @change="setActiveReader(reader, $event)"><option value="">{{ t('cards.noActive') }}</option><option v-for="(card, index) in cardManager.cards" :key="card.id + '-' + index" :value="card.id">{{ card.name }} · {{ card.id }}</option><option v-if="managerActive[reader] && !cardManager.cards.some(card => card.id === managerActive[reader]) && !cardManager.read_only?.some(card => card.id === managerActive[reader])" :value="managerActive[reader]">{{ managerActive[reader] }}</option><option v-for="card in cardManager.read_only" :key="card.source" :value="card.id">{{ card.name }} · {{ card.id }} ({{ card.source }})</option></select></label><button v-if="managerActive[reader]" class="quiet compact" :disabled="!!busy || !connected" @click="insert(managerActive[reader], reader)">{{ t('cards.managerInsert', { n: reader + 1 }) }}</button></div>
              <div v-for="{ card, index } in visibleManagerCards" :key="card.id + '-' + index" class="manager-card">
                <input v-model="card.name" :aria-label="t('cards.cardName')" maxlength="256" :disabled="!!busy" @input="managerDirty = true"><input v-model="card.id" :aria-label="t('cards.cardId')" maxlength="16" autocapitalize="characters" :disabled="!!busy" @input="managerDirty = true"><input type="color" :value="optionColor(card)" :aria-label="t('cards.cardColor')" :disabled="!!busy" @input="setManagerColor(card, $event)">
                <div class="button-row"><button v-for="reader in readers" :key="reader" class="quiet compact" :disabled="!!busy || !featureReady || managerDirty" @click="c.loadAndInsertCard(reader, card.id)">{{ t('cards.loadInsert', { n: reader + 1 }) }}</button><button class="quiet compact" @click="copyText(card.id)">{{ t('cards.copy') }}</button><button class="quiet compact" :disabled="!!busy" @click="randomizeCardColor(card)">{{ t('cards.randomColor') }}</button><button class="quiet compact" :aria-label="t('cards.cardName') + ': ' + card.name + ' ↑'" :disabled="index === 0 || !!busy" @click="moveManagerCard(index, -1)">↑</button><button class="quiet compact" :aria-label="t('cards.cardName') + ': ' + card.name + ' ↓'" :disabled="index === managerDraft.length - 1 || !!busy" @click="moveManagerCard(index, 1)">↓</button><button class="quiet compact" :disabled="!!busy" @click="deleteManagerCard(index)">{{ t('cards.delete') }}</button></div>
              </div>
              <p v-if="!visibleManagerCards.length && !visibleReadOnlyCards.length" class="muted small">{{ t('cards.empty') }}</p>
              <div v-for="card in visibleReadOnlyCards" :key="card.source" class="saved-card"><div><strong>{{ card.name }}</strong><code>{{ card.id }}</code><span class="small muted">{{ t('cards.readOnly') }} · {{ card.source }}</span></div><div class="button-row"><button v-for="reader in readers" :key="reader" class="quiet compact" :disabled="!!busy || !featureReady" @click="c.loadAndInsertCard(reader, card.id)">{{ t('cards.loadInsert', { n: reader + 1 }) }}</button><button class="quiet compact" @click="copyText(card.id)">{{ t('cards.copy') }}</button></div></div>
              <div class="panel-actions"><button :disabled="!featureReady || managerBaseRevision === undefined || !!busy" @click="addManagerCard">{{ t('cards.managerAdd') }}</button><span class="muted small">{{ t('cards.managerRevision', { n: cardManager.revision }) }}</span></div>
            </template>
            <p v-else class="muted small">{{ t('cards.managerHint') }}</p>
          </div>
        </div>
      </section>
      <section v-show="tab === 'keypad'" aria-labelledby="keypad-title">
        <div class="page-title"><h1 id="keypad-title">{{ t('nav.keypad') }}</h1></div>
        <div class="keypads-layout">
          <div v-for="reader in readers" :key="reader" class="settings-panel" role="group" :aria-label="'P' + (reader + 1)">
            <h2>P{{ reader + 1 }}</h2>
            <div v-if="keypadCapabilities?.keys !== false" class="keypad"><button v-for="key in ['7','8','9','4','5','6','1','2','3','0','A','D']" :key="key" :disabled="!connected || !!busy" @click="c.keypad(reader, key)">{{ key === 'A' ? '00' : key === 'D' ? '.' : key }}</button></div>
            <p v-else class="muted small">{{ t('keypad.unavailable') }}</p>
            <button class="keypad-insert" :disabled="!connected || !!busy || !keypadCapabilities?.insert_card" @click="c.insertKeypadCard(reader)">{{ t('keypad.insert') }}</button>
          </div>
        </div>
        <p v-if="!readers.length" class="empty-state">{{ t('keypad.unavailable') }}</p>
      </section>
      <section v-show="tab === 'preview'" aria-labelledby="preview-title">
        <div class="page-title"><h1 id="preview-title">{{ t('preview.title') }}</h1><div class="button-row"><select v-if="captureScreens.length" :value="selectedCaptureScreen" :aria-label="t('preview.title')" :disabled="capturing || !connected || demo || !!streamUrl || busy === 'stream'" @change="changeCaptureScreen"><option v-for="screen in captureScreens" :key="screen" :value="screen">{{ t('preview.screen', { n: screen + 1 }) }}</option></select><button class="quiet compact" :disabled="!connected || demo || capturing || !!streamUrl || !!busy" @click="refreshCapture">{{ t(capturing ? 'screen.capturing' : 'screen.refreshCapture') }}</button><a v-if="image && !streamUrl" class="button compact" :href="image" download="spice2x-capture.jpg">{{ t('preview.download') }}</a></div></div>
        <div class="settings-panel feature-toolbar">
          <label class="stacked-field">{{ t('preview.quality') }}<input v-model.number="captureQuality" type="number" min="1" max="100" step="1" :disabled="capturing || !!streamUrl"></label>
          <label class="stacked-field">{{ t('preview.size') }}<select :value="streamUrl ? 1 : captureDivide" :disabled="capturing || !!streamUrl" @change="captureDivide = Number(($event.target as HTMLSelectElement).value)"><option :value="1">{{ t('preview.fullSize') }}</option><option :value="2">1/2</option><option :value="4">1/4</option><option :value="8">1/8</option></select></label>
          <label class="stacked-field">{{ t('preview.fps') }}<input v-model.number="captureFps" type="number" min="1" max="60" step="1" :disabled="!!streamUrl"></label>
        </div>
        <p class="muted small">{{ t(streamUrl ? 'preview.streamHint' : 'preview.hint') }}</p>
        <p v-if="captureError" class="error-text" role="alert">{{ captureError }}</p>
        <CapturePlayer :stream-url="streamUrl" :stream-pending="busy === 'stream'" :image="image" :play-disabled="!connected || demo || !!busy || capturing" :active="tab === 'preview' && connected" @play="startStream" @stop="stopPreview" @error="streamFailed" />
        <p v-if="captureFrame && !streamUrl" class="muted small numeric">{{ captureFrame.width }} × {{ captureFrame.height }} · {{ imageTime }}</p>
      </section>
      <section v-show="tab === 'connection'" aria-labelledby="system-title">
        <div class="page-title"><h1 id="system-title">{{ t('connection.settings') }}</h1><button v-if="!embedded" class="quiet compact" :disabled="!!busy || status === 'connecting'" @click="addProfile">{{ t('connection.add') }}</button></div>
        <div class="connection-layout"><form class="settings-panel" @submit.prevent="connect"><fieldset :disabled="!!busy || status === 'connecting'"><label class="stacked-field">{{ t('connection.name') }}<input v-model="draft.name" maxlength="60" required autocomplete="off"></label><template v-if="!embedded"><label class="stacked-field">{{ t('connection.endpoint') }}<input v-model="draft.endpoint" placeholder="ws://192.168.1.10:1338" type="url" required spellcheck="false" autocomplete="off" autocapitalize="none"></label><p class="muted small">{{ t('connection.portHint') }}</p></template><label class="stacked-field">{{ t('connection.password') }}<input v-model="draft.password" type="password" autocomplete="off" :placeholder="t('connection.passwordPlaceholder')"></label><label class="check"><input v-model="draft.rememberPassword" type="checkbox">{{ t('connection.remember') }}</label><p class="muted small" v-if="draft.rememberPassword">{{ t('connection.rememberHint') }}</p><div class="panel-actions"><button type="button" @click="saveProfile">{{ t('connection.save') }}</button><button class="primary" type="submit">{{ t(status === 'connecting' ? 'status.connecting' : 'connection.connect') }}</button></div></fieldset></form>
          <aside class="connection-help"><details open><summary>{{ t('connection.first') }}</summary><ol><li>{{ t('connection.help1') }}</li><li>{{ t('connection.help2') }}</li><li>{{ t('connection.help3') }}</li></ol><p>{{ t('connection.browserHint') }}</p><p class="small muted">{{ t('connection.directHint') }}</p></details><button :disabled="!!busy || status === 'connecting'" @click="c.startDemo(); tab = 'resize'; showGrid = true">{{ t('screen.tryDemo') }}</button></aside>
        </div>
      </section>
      <section v-show="tab === 'log'" class="log-panel" aria-labelledby="log-title">
        <div class="page-title"><h1 id="log-title">{{ t('log.title') }}</h1><div class="button-row">
          <button class="quiet compact" :disabled="!featureReady || !!busy" @click="c.readLog()">{{ t('log.refresh') }}</button>
          <button class="quiet compact" :disabled="!featureReady || !!busy" @click="downloadLog()">{{ t('log.download') }}</button>
        </div></div>
        <div class="settings-panel">
          <div class="feature-toolbar"><label class="stacked-field">{{ t('log.search') }}<input v-model="logSearch" type="search"></label><label class="check"><input type="checkbox" :checked="logPolling" :disabled="!featureReady" @change="($event.target as HTMLInputElement).checked ? c.startLogPolling() : c.stopLogPolling()">{{ t('log.live') }}</label><label class="check"><input v-model="logAutoScroll" type="checkbox">{{ t('log.autoscroll') }}</label><button class="quiet compact" :disabled="!logText" @click="copyText(logLines.join('\n'))">{{ t('log.copy') }}</button><span class="small muted">{{ t('log.size', { n: logSize }) }}</span></div>
          <p class="small muted">{{ t('log.hint') }}</p>
          <pre ref="logViewer" tabindex="0" :aria-label="t('log.title')"><template v-if="logText"><span v-for="(line, index) in logLines" :key="index" :class="logLevel(line)">{{ line }}{{ index < logLines.length - 1 ? '\n' : '' }}</span></template><template v-else>{{ t('log.empty') }}</template></pre>
        </div>
      </section>
      <section v-show="tab === 'status'" aria-labelledby="status-title">
        <div class="page-title"><h1 id="status-title">{{ t('status.title') }}</h1><button class="quiet compact" :disabled="!!busy || !connected" @click="c.readStatus">{{ t('status.refresh') }}</button></div>
        <div v-if="featureErrors.status" class="empty-state"><h2>{{ t('status.unavailable') }}</h2><p>{{ featureErrors.status }}</p></div>
        <div v-else class="status-grid">
          <div class="settings-panel"><h2>{{ t('status.connection') }}</h2><dl class="status-list"><div><dt>{{ t('status.game') }}</dt><dd>{{ game || '—' }}</dd></div><div><dt>{{ t('status.gameIdentifier') }}</dt><dd>{{ [avsInfo.model, avsInfo.dest, avsInfo.spec, avsInfo.rev, avsInfo.ext].filter(Boolean).join(':') || '—' }}</dd></div><div><dt>{{ t('status.services') }}</dt><dd>{{ avsInfo.services || '—' }}</dd></div><div><dt>{{ t('status.version') }}</dt><dd>{{ version || '—' }}</dd></div><div><dt>{{ t('status.buildDate') }}</dt><dd>{{ [launcherInfo.compile_date, launcherInfo.compile_time].filter(Boolean).join(' ') || '—' }}</dd></div><div><dt>{{ t('status.systemTime') }}</dt><dd>{{ launcherInfo.system_time || '—' }}</dd></div><div><dt>{{ t('status.latency') }}</dt><dd>{{ latency ? `${latency} ms` : '—' }}</dd></div></dl></div>
          <div class="settings-panel"><h2>{{ t('status.memory') }}</h2><dl class="status-list"><div><dt>{{ t('status.processMemory') }} · {{ t('status.memoryUsed') }}</dt><dd>{{ memory ? formatBytes(memory.mem_used) : '—' }}</dd></div><div><dt>{{ t('status.processMemory') }} · {{ t('status.virtualUsed') }}</dt><dd>{{ memory ? formatBytes(memory.vmem_used) : '—' }}</dd></div><div><dt>{{ t('status.systemMemory') }} · {{ t('status.memoryUsed') }}</dt><dd>{{ memory ? formatBytes(memory.mem_total_used) : '—' }} / {{ memory ? formatBytes(memory.mem_total) : '—' }}</dd></div><div><dt>{{ t('status.systemMemory') }} · {{ t('status.virtualUsed') }}</dt><dd>{{ memory ? formatBytes(memory.vmem_total_used) : '—' }} / {{ memory ? formatBytes(memory.vmem_total) : '—' }}</dd></div></dl></div>
        </div>
      </section>
    </main>
    <footer class="app-footer"><div v-if="error || message" class="feedback" :class="{ 'has-error': !!error }" :role="error ? 'alert' : 'status'" aria-live="polite">{{ error || message }}</div><div class="footer-meta">version {{ buildVersion }}</div></footer>
  </div>
</template>
