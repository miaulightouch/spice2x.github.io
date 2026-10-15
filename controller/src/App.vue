<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref } from 'vue';
import { useController } from './useController';
import { isRecord, normalizeEndpoint } from './api/client';
import { defaultScene } from './api/resize';
import type { ResizePatch } from './api/resize';
import { loadCards, loadProfiles, normalizeCard, saveCards, saveProfiles } from './storage';
import type { Profile } from './storage';

const c = useController();
const { status, demo, connected, busy, error, message, game, version, latency, resize, resizeError,
  currentScene, applying, unsaved, image, imageTime, capturing, configuredPassword, coinQueue } = c;
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
const modeLabel = computed(() => demo.value ? '示範模式' : ({ disconnected: '尚未連線', connecting: '連線中…', connected: '已連線', error: '連線中斷' })[status.value]);
const scalePercent = (axis: 'x' | 'y') => Math.round(currentScene.value[axis === 'x' ? 'scale_x' : 'scale_y'] * 1000) / 10;
const controlsDisabled = computed(() => !connected.value || !!busy.value || !resize.value);
const boardStyle = computed(() => ({ transform: resize.value?.enabled ? `translate(${currentScene.value.offset_x}px, ${currentScene.value.offset_y}px) scale(${currentScene.value.scale_x}, ${currentScene.value.scale_y})` : 'none' }));
const position = computed(() => `X ${signed(currentScene.value.offset_x)} px · Y ${signed(currentScene.value.offset_y)} px`);
function signed(value: number): string { return `${value >= 0 ? '+' : ''}${value}`; }

function persistProfiles(): boolean {
  try { saveProfiles(profiles.value); return true; }
  catch { c.report(new Error('瀏覽器無法儲存設定；本次輸入仍可使用。')); return false; }
}
function changeProfile(): void {
  c.disconnect(); Object.assign(draft, selected.value); machineCards.value = [];
  showGrid.value = true; c.notify(`已選擇 ${selected.value.name}，請連線。`);
}
function applyProfile(persist = true): boolean {
  try {
    const endpoint = normalizeEndpoint(draft.endpoint);
    if (!draft.name.trim()) throw new Error('請填入機台名稱。');
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
  if (!saved && connected.value) c.report(new Error('已連線，但瀏覽器無法保留設定；關閉頁面後需重新輸入。'));
}
function saveProfile(): void {
  if (applyProfile()) { c.disconnect(); c.notify('連線設定已儲存在這個瀏覽器。'); }
}
function addProfile(): void {
  c.disconnect();
  const profile = { id: newId(), name: `機台 ${profiles.value.length + 1}`, endpoint: '', password: '', rememberPassword: false };
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
    else cards.value.push({ id: newId(), name: cardName.value.trim() || `卡片 ${cards.value.length + 1}`, number });
    saveCards(cards.value); c.notify('卡片已儲存在這個瀏覽器。');
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
    c.notify(machineCards.value.length ? '已讀取機台設定的卡片。' : '機台目前沒有設定卡片。');
  } catch (value) { c.report(value); }
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
      <a class="brand" href="#" @click.prevent="tab = 'screen'" aria-label="Arcade Room 畫面校正">
        <span class="brand-mark" aria-hidden="true"><i v-for="n in 4" :key="n"></i></span>
        <span><strong>ARCADE ROOM</strong><small>Web controller</small></span>
      </a>
      <div class="machine-controls">
        <label class="sr-only" for="machine">選擇機台</label>
        <select id="machine" v-model="selectedId" :disabled="!!busy || status === 'connecting'" @change="changeProfile"><option v-for="p in profiles" :key="p.id" :value="p.id">{{ p.name }}</option></select>
        <span class="connection-chip" :data-status="demo ? 'demo' : status"><i aria-hidden="true"></i>{{ modeLabel }}</span>
        <button v-if="connected || status === 'connecting'" class="quiet compact" :disabled="!!busy" @click="c.disconnect(); c.notify('已中斷連線')">中斷</button>
        <button v-else class="primary compact" @click="tab = 'system'">連線設定</button>
      </div>
    </header>

    <div v-if="demo" class="demo-strip">示範模式 · 所有操作只在這個頁面模擬，不會送到機台。<button class="text-button" @click="c.disconnect(); tab = 'system'">連線至機台</button></div>
    <div v-if="offline" class="offline-strip">瀏覽器回報網路離線；已快取的介面仍可開啟，機台連線取決於區域網路狀態。</div>

    <nav class="main-nav" aria-label="控制器功能">
      <button v-for="item in [{ key: 'operate', label: '操作', symbol: '◉' }, { key: 'cards', label: '卡片', symbol: '▤' }, { key: 'screen', label: '畫面', symbol: '▦' }, { key: 'system', label: '連線', symbol: '⌁' }]" :key="item.key" :aria-pressed="tab === item.key" @click="tab = item.key"><span aria-hidden="true">{{ item.symbol }}</span>{{ item.label }}</button>
    </nav>

    <main class="main-content">
      <section v-show="tab === 'screen'" aria-labelledby="screen-title">
        <div class="page-title"><div><p class="eyebrow">SCREEN CALIBRATION</p><h1 id="screen-title">畫面校正</h1></div><span class="muted small" v-if="resize">{{ applying ? '套用中…' : unsaved ? '已套用 · 尚未儲存' : '機台設定' }}</span></div>
        <div v-if="!connected" class="empty-state"><h2>將畫面調整到剛剛好</h2><p>連線後可直接調整四組場景的縮放與位移。</p><div class="button-row"><button class="primary" @click="tab = 'system'">設定連線</button><button @click="c.startDemo()">試用示範模式</button></div></div>
        <div v-else-if="resizeError" class="empty-state"><h2>畫面校正暫不可用</h2><p>{{ resizeError }}</p><p class="small">新校正功能需要 Resize API 版本，並在遊戲初始化後使用。</p><div class="button-row"><button @click="c.readResize()" :disabled="!!busy">重新讀取</button><a class="button" href="https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-16-resize-api.1" target="_blank" rel="noopener noreferrer">下載 API 版本</a></div></div>
        <div v-else-if="resize" class="calibration-layout">
          <div>
            <div class="section-toolbar"><h2>{{ !showGrid && image ? '機台畫面擷取' : '校正格線示意' }}</h2><label class="check"><input type="checkbox" :checked="resize.enabled" :disabled="controlsDisabled" @change="toggle('enabled', $event)">啟用縮放</label></div>
            <div class="screen-preview" :class="{ 'is-capture': !showGrid && image }">
              <template v-if="showGrid || !image"><div class="target-frame" aria-hidden="true"></div><div class="calibration-board" :style="boardStyle" role="img" aria-label="四乘四校正格線示意"><div v-for="n in 16" :key="n">{{ String(n).padStart(2, '0') }}</div></div><span class="preview-caption">格線僅為示意，實際效果請看機台畫面</span></template>
              <img v-else :src="image" alt="機台最後一次擷取的畫面">
            </div>
            <div class="preview-toolbar"><button class="quiet compact" :disabled="capturing || demo" @click="capture">{{ capturing ? '擷取中…' : '更新機台畫面' }}</button><button v-if="image" class="text-button" @click="showGrid = !showGrid">{{ showGrid ? '顯示機台擷取' : '顯示校正格線' }}</button><span class="muted small" v-if="imageTime">{{ imageTime }} 擷取</span></div>

            <div class="position-controls">
              <div class="position-values"><span class="muted small">位移</span><output class="numeric">{{ position }}</output><label class="inline-field">步距<select v-model.number="step"><option :value="1">1 px</option><option :value="5">5 px</option><option :value="10">10 px</option></select></label></div>
              <div class="dpad" role="group" aria-label="畫面位移">
                <button data-move="up" aria-label="向上微調" :disabled="controlsDisabled" @click="move('up')">↑</button>
                <button data-move="left" aria-label="向左微調" :disabled="controlsDisabled" @click="move('left')">←</button>
                <button data-move="center" aria-label="位移歸零" :disabled="controlsDisabled" @click="move('center')">○</button>
                <button data-move="right" aria-label="向右微調" :disabled="controlsDisabled" @click="move('right')">→</button>
                <button data-move="down" aria-label="向下微調" :disabled="controlsDisabled" @click="move('down')">↓</button>
              </div>
            </div>
            <div class="offset-inputs"><label>X 位移（px）<input type="number" min="-32768" max="32767" step="1" :value="currentScene.offset_x" :disabled="controlsDisabled" @change="inputOffset('offset_x', $event)"></label><label>Y 位移（px）<input type="number" min="-32768" max="32767" step="1" :value="currentScene.offset_y" :disabled="controlsDisabled" @change="inputOffset('offset_y', $event)"></label></div>
          </div>
          <div class="settings-panel">
            <div class="section-toolbar"><h2>縮放場景</h2><span class="muted small">4 組預設</span></div>
            <div class="scene-picker" role="group" aria-label="縮放場景"><button v-for="n in 4" :key="n" :aria-pressed="resize.scene === n" :disabled="controlsDisabled" @click="c.selectScene(n)">場景 {{ n }}</button></div>
            <div v-for="axis in (['x', 'y'] as const)" :key="axis" class="scale-field">
              <div class="field-header"><label :for="'scale-' + axis">{{ axis === 'x' ? '水平縮放' : '垂直縮放' }}</label><output class="numeric">{{ scalePercent(axis).toFixed(1) }}%</output></div>
              <input :id="'scale-' + axis" type="range" min="60" max="250" step="0.1" :value="scalePercent(axis)" :disabled="controlsDisabled" @input="inputScale(axis, $event)">
              <div class="number-stepper"><button :aria-label="(axis === 'x' ? '水平' : '垂直') + '縮小 0.1%'" :disabled="controlsDisabled" @click="scale(axis, scalePercent(axis) - 0.1)">−</button><input type="number" min="60" max="250" step="0.1" :aria-label="(axis === 'x' ? '水平' : '垂直') + '縮放百分比'" :value="scalePercent(axis)" :disabled="controlsDisabled" @change="inputScale(axis, $event)"><button :aria-label="(axis === 'x' ? '水平' : '垂直') + '放大 0.1%'" :disabled="controlsDisabled" @click="scale(axis, scalePercent(axis) + 0.1)">＋</button></div>
            </div>
            <label class="check"><input type="checkbox" :checked="currentScene.keep_aspect_ratio" :disabled="controlsDisabled" @change="toggle('keep_aspect_ratio', $event)">等比例縮放</label>
            <label class="check"><input type="checkbox" :checked="resize.linear_filter" :disabled="controlsDisabled" @change="toggle('linear_filter', $event)">線性濾波</label>
            <label class="stacked-field duplicate-field">畫面複製<select :value="currentScene.duplicate" :disabled="controlsDisabled" @change="c.updateResize({ duplicate: Number(($event.target as HTMLSelectElement).value) })"><option :value="0">不複製</option><option :value="1">複製左側</option><option :value="2">複製右側</option></select></label>
            <div class="panel-actions"><button :disabled="controlsDisabled" @click="resetScene">重設此場景</button><button class="primary" :disabled="controlsDisabled || (!demo && !configuredPassword)" @click="c.saveResize()">{{ busy === 'save' ? '儲存中…' : '儲存到機台' }}</button></div>
            <p class="muted small" v-if="!demo && !configuredPassword">儲存到機台需要設定 API 密碼。</p>
            <button class="text-button read-action" :disabled="controlsDisabled" @click="c.readResize()">重新讀取機台設定</button>
          </div>
        </div>
        <p v-else class="muted">正在讀取機台設定…</p>
      </section>

      <section v-show="tab === 'operate'" aria-labelledby="operate-title">
        <div class="page-title"><div><p class="eyebrow">MACHINE CONTROLS</p><h1 id="operate-title">機台操作</h1></div></div>
        <div class="operation-grid"><button class="operation-button" :disabled="!connected || !!busy || !c.exactButton('Service')" @click="c.pulse('Service')"><span>SERVICE</span><small>點按一次，自動放開</small></button><button class="operation-button" :disabled="!connected || !!busy || !c.exactButton('Test')" @click="c.pulse('Test')"><span>TEST</span><small>點按一次，自動放開</small></button><button class="operation-button" :disabled="!connected || !!busy" @click="c.insertCoin()"><span>投幣 ＋1</span><small>加入機台投幣佇列</small></button><button class="operation-button" :disabled="!connected || !!busy" @click="c.releaseButtons()"><span>解除按鍵覆寫</span><small>放開所有 API 按鍵</small></button></div>
        <p class="muted small" v-if="coinQueue !== undefined">尚未處理的投幣：{{ coinQueue }}（與遊戲內餘額不同）</p>
        <p class="muted" v-if="!connected">連線後即可使用機台操作。</p>
        <p class="muted small" v-else-if="!c.exactButton('Service') || !c.exactButton('Test')">未提供的按鍵會停用；可用按鍵由機台 API 回報。</p>
      </section>

      <section v-show="tab === 'cards'" aria-labelledby="cards-title">
        <div class="page-title"><div><p class="eyebrow">CARDS & KEYPAD</p><h1 id="cards-title">卡片與數字鍵</h1></div><label class="inline-field">讀卡器<select v-model.number="unit"><option :value="0">P1</option><option :value="1">P2</option></select></label></div>
        <div class="cards-layout"><div>
          <form class="settings-panel" @submit.prevent="insert()"><h2>插入卡片</h2><label class="stacked-field">卡號<input v-model="cardNumber" class="card-number" placeholder="E004 0000 0000 0001" maxlength="32" autocapitalize="characters" spellcheck="false" autocomplete="off"></label><label class="stacked-field">名稱（選填）<input v-model="cardName" placeholder="例如：我的卡片" maxlength="60" autocomplete="off"></label><div class="panel-actions"><button type="button" @click="saveCard">儲存在瀏覽器</button><button type="submit" class="primary" :disabled="!connected || !!busy || !cardNumber.trim()">插入 P{{ unit + 1 }}</button></div></form>
          <div class="saved-cards"><h2>我的卡片</h2><p class="muted small" v-if="!cards.length">儲存的卡片只會留在這個瀏覽器。</p><div v-for="card in cards" :key="card.id" class="saved-card"><div><strong>{{ card.name }}</strong><code>{{ card.number.match(/.{1,4}/g)?.join(' ') }}</code></div><div class="button-row"><button :disabled="!connected || !!busy" @click="insert(card.number)">刷卡</button><button class="quiet" :aria-label="'刪除 ' + card.name" @click="removeCard(card.id)">刪除</button></div></div></div>
          <button class="text-button" :disabled="!connected || (!demo && !configuredPassword)" @click="readCards">讀取機台預設卡片</button><div v-for="card in machineCards" :key="card.index" class="machine-card"><span>P{{ card.index + 1 }} · {{ card.card_id }}</span><button class="quiet compact" @click="cardNumber = card.card_id">填入卡號</button></div>
        </div><div class="settings-panel keypad-panel"><h2>數字鍵</h2><p class="muted small">送至 P{{ unit + 1 }} 數字鍵盤</p><div class="keypad"><button v-for="key in ['1','2','3','4','5','6','7','8','9','A','0','D']" :key="key" :disabled="!connected || !!busy" @click="c.keypad(unit, key)">{{ key === 'A' ? '00' : key === 'D' ? '.' : key }}</button></div></div></div>
      </section>

      <section v-show="tab === 'system'" aria-labelledby="system-title">
        <div class="page-title"><div><p class="eyebrow">CONNECTION</p><h1 id="system-title">連線設定</h1></div><button class="quiet compact" :disabled="!!busy || status === 'connecting'" @click="addProfile">新增機台</button></div>
        <div class="connection-layout"><form class="settings-panel" @submit.prevent="connect">
          <fieldset :disabled="!!busy || status === 'connecting'"><label class="stacked-field">機台名稱<input v-model="draft.name" maxlength="60" required autocomplete="off"></label><label class="stacked-field">WebSocket 位址<input v-model="draft.endpoint" placeholder="ws://192.168.1.10:1338" type="url" required spellcheck="false" autocomplete="off" autocapitalize="none"></label><p class="muted small">API 連接埠加 1：例如 API 1337 → WebSocket 1338。</p><label class="stacked-field">API 密碼<input v-model="draft.password" type="password" autocomplete="off" placeholder="與 spice2x 設定相同；未設定可留空"></label><label class="check"><input v-model="draft.rememberPassword" type="checkbox">在這個瀏覽器記住密碼</label><p class="muted small" v-if="draft.rememberPassword">密碼會保存在這個瀏覽器的網站儲存空間。</p><div class="panel-actions"><button type="button" @click="saveProfile">儲存設定</button><button class="primary" type="submit">{{ status === 'connecting' ? '連線中…' : '連線至機台' }}</button></div></fieldset>
        </form><aside class="connection-help"><h2>第一次連線</h2><ol><li>在 spice2x 開啟 API，確認 WebSocket 連接埠。</li><li>讓手機或電腦與機台位於可互通的網路。</li><li>按下連線，並允許瀏覽器存取本機網路。</li></ol><p>建議使用新版 Chrome 或 Edge。其他瀏覽器可改用提供 TLS 的 <code>wss://</code> 入口。</p><p class="small muted">頁面只負責載入控制器，操作會從瀏覽器直接送到你指定的機台。</p><button :disabled="!!busy || status === 'connecting'" @click="c.startDemo(); showGrid = true">先試用示範模式</button></aside></div>
      </section>
    </main>

    <footer class="app-footer"><div class="feedback" :class="{ 'has-error': !!error }" :role="error ? 'alert' : 'status'" aria-live="polite">{{ error || message }}</div><div class="footer-meta" v-if="connected && !demo"><span>{{ [game, version].filter(Boolean).join(' · ') || selected.name }}</span><span v-if="latency">{{ latency }} ms</span></div><div class="footer-meta" v-else><span>Arcade Room 0.1</span><span>WebSocket · RWD</span></div></footer>
  </div>
</template>
