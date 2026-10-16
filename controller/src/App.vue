<script setup lang="ts">
import { t, msg, locale, languages, setLocale, LocalizedError } from './i18n';
import { computed, onBeforeUnmount, reactive, ref } from 'vue';
import { useController } from './useController';
import { isRecord, normalizeEndpoint } from './api/client';
import { defaultScene } from './api/resize';
import type { ResizePatch } from './api/resize';
import { loadCards, loadProfiles, normalizeCard, saveCards, saveProfiles } from './storage';
import type { Locale } from './i18n';
import type { Profile } from './storage';

const c = useController();
const { status, demo, connected, busy, error, message, game, version, latency, resize, resizeError,
  currentScene, applying, unsaved, image, imageTime, capturing, configuredPassword, coinQueue, logText, logSize } = c;
const logSearch = ref('');
const filteredLog = computed(() => logSearch.value ? logText.value.split('\n').filter(line => line.toLowerCase().includes(logSearch.value.toLowerCase())).join('\n') : logText.value);
async function downloadLog(): Promise<void> {
  const file = await c.downloadLog();
  if (!file) return;
  const url = URL.createObjectURL(file);
  const link = document.createElement('a'); link.href = url; link.download = 'log.txt';
  document.body.appendChild(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
const profiles = ref(loadProfiles());
const selectedId = ref(profiles.value[0]!.id);
const selected = computed(() => profiles.value.find(p => p.id === selectedId.value)!);
const draft = reactive<Profile>({ ...selected.value });
const tab = ref('screen');
const step = ref(1);
const cards = ref(loadCards());
const cardNumber = ref('');
const cardName = ref('');
const unit = ref(0);
const machineCards = ref<{ index: number; card_id: string }[]>([]);
const showGrid = ref(true);
const offline = ref(!navigator.onLine);
const modeLabel = computed(() => demo.value ? t('status.demo') : ({ disconnected: t('status.disconnected'), connecting: t('status.connecting'), connected: t('status.connected'), error: t('status.error') })[status.value]);
const scalePercent = (axis: 'x' | 'y') => Math.round(currentScene.value[axis === 'x' ? 'scale_x' : 'scale_y'] * 1000) / 10;
const controlsDisabled = computed(() => !connected.value || !!busy.value || !resize.value);
const boardStyle = computed(() => ({ transform: resize.value?.enabled ? `translate(${currentScene.value.offset_x}px, ${currentScene.value.offset_y}px) scale(${currentScene.value.scale_x}, ${currentScene.value.scale_y})` : 'none' }));
const position = computed(() => `X ${signed(currentScene.value.offset_x)} px · Y ${signed(currentScene.value.offset_y)} px`);
function signed(value: number): string { return `${value >= 0 ? '+' : ''}${value}`; }

function persistProfiles(): boolean {
  try { saveProfiles(profiles.value); return true; }
  catch { c.report(new LocalizedError('error.storage')); return false; }
}
function changeProfile(): void {
  c.disconnect(); Object.assign(draft, selected.value); machineCards.value = [];
  showGrid.value = true; c.notify(msg('msg.selected', { name: selected.value.name }));
}
function applyProfile(persist = true): boolean {
  try {
    const endpoint = normalizeEndpoint(draft.endpoint);
    if (!draft.name.trim()) throw new LocalizedError('error.name');
    const saved = { ...draft, endpoint, name: draft.name.trim() };
    const index = profiles.value.findIndex(p => p.id === selectedId.value);
    profiles.value[index] = saved; Object.assign(draft, saved);
    return persist ? persistProfiles() : true;
  } catch (value) { c.report(value); return false; }
}
async function connect(): Promise<void> {
  if (!applyProfile(false)) return;
  const saved = persistProfiles();
  machineCards.value = []; showGrid.value = true;
  await c.connect({ ...selected.value });
  if (!saved && connected.value) c.report(new LocalizedError('error.profileNotSaved'));
}
function saveProfile(): void {
  if (applyProfile()) { c.disconnect(); c.notify(msg('msg.profileSaved')); }
}
function addProfile(): void {
  c.disconnect();
  const profile = { id: newId(), name: t('msg.defaultCabinet', { n: profiles.value.length + 1 }), endpoint: '', password: '', rememberPassword: false };
  profiles.value.push(profile); selectedId.value = profile.id; Object.assign(draft, profile);
  persistProfiles(); tab.value = 'system';
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
async function insert(number = cardNumber.value): Promise<void> {
  try { await c.insertCard(unit.value, normalizeCard(number)); } catch (value) { c.report(value); }
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
const systemActions = computed(() => [
  { command: 'restart', label: t('action.restart'), detail: t('action.restartDetail') },
  { command: 'exit', label: t('action.exit'), detail: t('action.exitDetail') },
  { command: 'reboot', label: t('action.reboot'), detail: t('action.rebootDetail') },
  { command: 'shutdown', label: t('action.shutdown'), detail: t('action.shutdownDetail') },
] as const);
function confirmSystem(command: 'restart' | 'exit' | 'reboot' | 'shutdown'): void {
  const item = systemActions.value.find(item => item.command === command)!;
  const warning = unsaved.value ? '\n' + t('action.unsaved') : '';
  const machine = profiles.value.find(item => item.id === selectedId.value)?.name ?? t('msg.cabinet');
  const prompt = t('action.confirm', { machine, action: item.label, detail: item.detail });
  if (window.confirm((demo.value ? t('action.demoPrefix') : '') + prompt + warning)) void c.machineControl(command);
}
function updateOnline(): void { offline.value = !navigator.onLine; }
function newId(): string { return globalThis.crypto?.randomUUID?.() ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`; }
window.addEventListener('online', updateOnline); window.addEventListener('offline', updateOnline);
function onPageHide(): void { c.disconnect(); }
window.addEventListener('pagehide', onPageHide);
onBeforeUnmount(() => {
  c.disconnect(); window.removeEventListener('online', updateOnline); window.removeEventListener('offline', updateOnline);
  window.removeEventListener('pagehide', onPageHide);
});
</script>

<template>
  <div class="controller-app">
    <header class="app-header">
      <a class="brand" href="#" @click.prevent="tab = 'screen'" :aria-label="t('app.home')">
        <span class="brand-mark" aria-hidden="true"><i v-for="n in 4" :key="n"></i></span>
        <span><strong>ARCADE ROOM</strong><small>{{ t('app.subtitle') }}</small></span>
      </a>
      <label class="language-control"><span class="sr-only">{{ t('app.language') }}</span><select :value="locale" @change="setLocale(($event.target as HTMLSelectElement).value as Locale)"><option v-for="language in languages" :key="language.code" :value="language.code" :lang="language.code">{{ language.label }}</option></select></label>
      <div class="machine-controls">
        <label class="sr-only" for="machine">{{ t('connection.select') }}</label>
        <select id="machine" v-model="selectedId" :disabled="!!busy || status === 'connecting'" @change="changeProfile"><option v-for="p in profiles" :key="p.id" :value="p.id">{{ p.name }}</option></select>
        <span class="connection-chip" :data-status="demo ? 'demo' : status"><i aria-hidden="true"></i>{{ modeLabel }}</span>
        <button v-if="connected || status === 'connecting'" class="quiet compact" :disabled="!!busy" @click="c.disconnect(); c.notify(msg('msg.disconnected'))">{{ t('connection.disconnect') }}</button>
        <button v-else class="primary compact" @click="tab = 'system'">{{ t('connection.settings') }}</button>
      </div>
    </header>
    <div v-if="demo" class="demo-strip">{{ t('connection.demoStrip') }}<button class="text-button" @click="c.disconnect(); tab = 'system'">{{ t('connection.connect') }}</button></div>
    <div v-if="offline" class="offline-strip">{{ t('connection.offline') }}</div>
    <nav class="main-nav" :aria-label="t('nav.label')">
      <button v-for="item in [{ key: 'operate', label: t('nav.operate'), symbol: '◉' }, { key: 'cards', label: t('nav.cards'), symbol: '▤' }, { key: 'screen', label: t('nav.screen'), symbol: '▦' }, { key: 'system', label: t('nav.system'), symbol: '⌁' }]" :key="item.key" :aria-pressed="tab === item.key" @click="tab = item.key"><span aria-hidden="true">{{ item.symbol }}</span>{{ item.label }}</button>
    </nav>
    <main class="main-content">
      <section v-show="tab === 'screen'" aria-labelledby="screen-title">
        <div class="page-title"><h1 id="screen-title">{{ t('screen.title') }}</h1><span class="muted small" v-if="resize">{{ applying ? t('screen.updating') : !resize.enabled ? t('screen.disabled') : unsaved ? t('screen.unsaved') : t('screen.saved') }}</span></div>
        <div v-if="!connected" class="empty-state"><h2>{{ t('screen.welcome') }}</h2><p>{{ t('screen.connectHint') }}</p><div class="button-row"><button class="primary" @click="tab = 'system'">{{ t('screen.setup') }}</button><button @click="c.startDemo()">{{ t('screen.tryDemo') }}</button></div></div>
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
              <div class="preview-toolbar"><button class="quiet compact" :disabled="capturing || demo" @click="capture">{{ t(capturing ? 'screen.capturing' : 'screen.refreshCapture') }}</button><button v-if="image" class="text-button" @click="showGrid = !showGrid">{{ t(showGrid ? 'screen.showCapture' : 'screen.showGrid') }}</button><span class="muted small" v-if="imageTime">{{ imageTime }} · {{ t('screen.still') }}</span></div>
            </details>
            <button class="text-button read-action" :disabled="controlsDisabled" @click="c.readResize()">{{ t('screen.reload') }}</button>
            <details class="usage-notes"><summary>{{ t('app.details') }}</summary><p>{{ t('screen.saveHint') }}</p><p>{{ t('screen.backendHint') }}</p></details>
          </div>
        </div><p v-else class="muted">{{ t('screen.loading') }}</p>
      </section>
      <section v-show="tab === 'operate'" aria-labelledby="operate-title">
        <div class="page-title"><h1 id="operate-title">{{ t('operate.title') }}</h1></div>
        <div class="operation-grid"><button class="operation-button" :disabled="!connected || !!busy || !c.exactButton('Service')" @click="c.pulse('Service')"><span>SERVICE</span><small>{{ t('operate.pulse') }}</small></button><button class="operation-button" :disabled="!connected || !!busy || !c.exactButton('Test')" @click="c.pulse('Test')"><span>TEST</span><small>{{ t('operate.pulse') }}</small></button><button class="operation-button" :disabled="!connected || !!busy" @click="c.insertCoin()"><span>{{ t('operate.coin') }}</span><small>{{ t('operate.coinHint') }}</small></button><button class="operation-button" :disabled="!connected || !!busy" @click="c.releaseButtons()"><span>{{ t('operate.release') }}</span><small>{{ t('operate.releaseHint') }}</small></button></div>
        <div class="settings-panel system-actions"><h2>{{ t('operate.power') }}</h2><div class="operation-grid"><button v-for="item in systemActions" :key="item.command" class="operation-button" :disabled="!connected || !!busy || (!demo && !configuredPassword)" @click="confirmSystem(item.command)"><span>{{ item.label }}</span><small>{{ item.detail }}</small></button></div><p v-if="!demo && !configuredPassword" class="muted small">{{ t('operate.passwordHint') }}</p><details class="usage-notes"><summary>{{ t('app.details') }}</summary><p>{{ t('operate.powerHint') }}</p></details></div>
        <p class="muted small" v-if="coinQueue !== undefined">{{ t('operate.queue', { n: coinQueue }) }}</p><p class="muted" v-if="!connected">{{ t('operate.connectHint') }}</p><p class="muted small" v-else-if="!c.exactButton('Service') || !c.exactButton('Test')">{{ t('operate.missingButtons') }}</p>
      </section>
      <section v-show="tab === 'cards'" aria-labelledby="cards-title">
        <div class="page-title"><h1 id="cards-title">{{ t('cards.title') }}</h1><label class="inline-field">{{ t('cards.reader') }}<select v-model.number="unit"><option :value="0">P1</option><option :value="1">P2</option></select></label></div>
        <div class="cards-layout"><div><form class="settings-panel" @submit.prevent="insert()"><h2>{{ t('cards.insertTitle') }}</h2><label class="stacked-field">{{ t('cards.number') }}<input v-model="cardNumber" class="card-number" placeholder="E004 0000 0000 0001" maxlength="32" autocapitalize="characters" spellcheck="false" autocomplete="off"></label><label class="stacked-field">{{ t('cards.name') }}<input v-model="cardName" :placeholder="t('cards.nameHint')" maxlength="60" autocomplete="off"></label><div class="panel-actions"><button type="button" @click="saveCard">{{ t('cards.save') }}</button><button type="submit" class="primary" :disabled="!connected || !!busy || !cardNumber.trim()">{{ t('cards.insert', { n: unit + 1 }) }}</button></div></form>
          <div class="saved-cards"><h2>{{ t('cards.mine') }}</h2><p class="muted small" v-if="!cards.length">{{ t('cards.localHint') }}</p><div v-for="card in cards" :key="card.id" class="saved-card"><div><strong>{{ card.name }}</strong><code>{{ card.number.match(/.{1,4}/g)?.join(' ') }}</code></div><div class="button-row"><button :disabled="!connected || !!busy" @click="insert(card.number)">{{ t('cards.use') }}</button><button class="quiet" :aria-label="t('cards.deleteLabel', { name: card.name })" @click="removeCard(card.id)">{{ t('cards.delete') }}</button></div></div></div>
          <button class="text-button" :disabled="!connected || (!demo && !configuredPassword)" @click="readCards">{{ t('cards.read') }}</button><div v-for="card in machineCards" :key="card.index" class="machine-card"><span>P{{ card.index + 1 }} · {{ card.card_id }}</span><button class="quiet compact" @click="cardNumber = card.card_id">{{ t('cards.fill') }}</button></div>
        </div><div class="settings-panel keypad-panel"><h2>{{ t('cards.keypad') }}</h2><p class="muted small">{{ t('cards.target', { n: unit + 1 }) }}</p><div class="keypad"><button v-for="key in ['1','2','3','4','5','6','7','8','9','A','0','D']" :key="key" :disabled="!connected || !!busy" @click="c.keypad(unit, key)">{{ key === 'A' ? '00' : key === 'D' ? '.' : key }}</button></div></div></div>
      </section>
      <section v-show="tab === 'system'" aria-labelledby="system-title">
        <div class="page-title"><h1 id="system-title">{{ t('connection.settings') }}</h1><button class="quiet compact" :disabled="!!busy || status === 'connecting'" @click="addProfile">{{ t('connection.add') }}</button></div>
        <div class="connection-layout"><form class="settings-panel" @submit.prevent="connect"><fieldset :disabled="!!busy || status === 'connecting'"><label class="stacked-field">{{ t('connection.name') }}<input v-model="draft.name" maxlength="60" required autocomplete="off"></label><label class="stacked-field">{{ t('connection.endpoint') }}<input v-model="draft.endpoint" placeholder="ws://192.168.1.10:1338" type="url" required spellcheck="false" autocomplete="off" autocapitalize="none"></label><p class="muted small">{{ t('connection.portHint') }}</p><label class="stacked-field">{{ t('connection.password') }}<input v-model="draft.password" type="password" autocomplete="off" :placeholder="t('connection.passwordPlaceholder')"></label><label class="check"><input v-model="draft.rememberPassword" type="checkbox">{{ t('connection.remember') }}</label><p class="muted small" v-if="draft.rememberPassword">{{ t('connection.rememberHint') }}</p><div class="panel-actions"><button type="button" @click="saveProfile">{{ t('connection.save') }}</button><button class="primary" type="submit">{{ t(status === 'connecting' ? 'status.connecting' : 'connection.connect') }}</button></div></fieldset></form>
          <aside class="connection-help"><details open><summary>{{ t('connection.first') }}</summary><ol><li>{{ t('connection.help1') }}</li><li>{{ t('connection.help2') }}</li><li>{{ t('connection.help3') }}</li></ol><p>{{ t('connection.browserHint') }}</p><p class="small muted">{{ t('connection.directHint') }}</p></details><button :disabled="!!busy || status === 'connecting'" @click="c.startDemo(); tab = 'screen'; showGrid = true">{{ t('screen.tryDemo') }}</button></aside>
        </div>
      </section>
      <section v-show="tab === 'system'" class="log-panel" aria-labelledby="log-title">
        <h2 id="log-title">{{ t('log.title') }}</h2>
        <p class="small muted">{{ t('log.hint') }}</p>
        <div class="button-row">
          <button :disabled="!connected || !!busy" @click="c.readLog()">{{ t('log.refresh') }}</button>
          <button :disabled="!connected || !!busy" @click="downloadLog()">{{ t('log.download') }}</button>
          <span class="small muted">{{ t('log.size', { n: logSize }) }}</span>
        </div>
        <label class="stacked-field">{{ t('log.search') }}<input v-model="logSearch" type="search"></label>
        <pre tabindex="0" :aria-label="t('log.title')">{{ filteredLog || t('log.empty') }}</pre>
      </section>
    </main>
    <footer class="app-footer"><div class="feedback" :class="{ 'has-error': !!error }" :role="error ? 'alert' : 'status'" aria-live="polite">{{ error || message }}</div><div class="footer-meta" v-if="connected && !demo"><span>{{ [game, version].filter(Boolean).join(' · ') || selected.name }}</span><span v-if="latency">{{ latency }} ms</span></div><div class="footer-meta" v-else><span>Arcade Room</span><span>SpiceAPI · WebSocket</span></div></footer>
  </div>
</template>
