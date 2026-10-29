<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { t } from '../i18n';

const props = defineProps<{ streamUrl: string; streamPending: boolean; image: string; playDisabled: boolean; active: boolean }>();
defineEmits<{ play: []; stop: []; error: [event: Event] }>();
const player = ref<HTMLElement>();
const streamImage = ref<HTMLImageElement>();
const nativeFullscreen = ref(false);
const expanded = ref(false);
const fullscreen = computed(() => nativeFullscreen.value || expanded.value);
let previousOverflow = '';

// Detach the source before removing the image, so multipart downloads stop immediately.
watch(() => props.streamUrl, (_value, previous) => {
  if (previous) streamImage.value?.removeAttribute('src');
}, { flush: 'sync' });
watch(expanded, value => {
  if (value) { previousOverflow = document.body.style.overflow; document.body.style.overflow = 'hidden'; }
  else document.body.style.overflow = previousOverflow;
}, { flush: 'sync' });

function syncFullscreen(): void { nativeFullscreen.value = document.fullscreenElement === player.value; }
async function leaveFullscreen(): Promise<void> {
  expanded.value = false;
  if (player.value && document.fullscreenElement === player.value) await document.exitFullscreen();
}
async function toggleFullscreen(): Promise<void> {
  if (fullscreen.value) { await leaveFullscreen(); return; }
  if (player.value?.requestFullscreen) {
    try { await player.value.requestFullscreen(); return; }
    catch { /* Embedded browsers may only support filling the browser viewport. */ }
  }
  expanded.value = true;
}
function escape(event: KeyboardEvent): void { if (event.key === 'Escape') expanded.value = false; }
watch(() => props.active, active => { if (!active) void leaveFullscreen().catch(() => {}); });
onMounted(() => {
  document.addEventListener('fullscreenchange', syncFullscreen);
  document.addEventListener('keydown', escape);
});
onBeforeUnmount(() => {
  streamImage.value?.removeAttribute('src');
  document.removeEventListener('fullscreenchange', syncFullscreen);
  document.removeEventListener('keydown', escape);
  void leaveFullscreen().catch(() => {});
});
</script>

<template>
  <div ref="player" class="capture-player" :class="{ expanded }">
    <div class="capture-image">
      <img v-if="streamUrl" ref="streamImage" :src="streamUrl" :alt="t('preview.title')" @error="$emit('error', $event)">
      <img v-else-if="image" :src="image" :alt="t('screen.capture')">
      <p v-else class="muted small">{{ t('preview.empty') }}</p>
    </div>
    <div class="player-controls">
      <button type="button" class="quiet compact" :disabled="!!streamUrl || playDisabled" @click="$emit('play')"><span aria-hidden="true">▶</span>{{ t('preview.play') }}</button>
      <button type="button" class="quiet compact" :disabled="!streamUrl && !streamPending" @click="$emit('stop')"><span aria-hidden="true">■</span>{{ t('preview.stop') }}</button>
      <button type="button" class="quiet compact fullscreen-button" :aria-pressed="fullscreen" @click="toggleFullscreen"><span aria-hidden="true">⛶</span>{{ t(fullscreen ? 'preview.exitFullscreen' : 'preview.fullscreen') }}</button>
    </div>
  </div>
</template>

<style scoped>
.capture-player { margin-block: 12px 6px; border: 1px solid var(--line); border-radius: 8px; background: var(--bg); overflow: hidden; }
.capture-image { display: grid; place-items: center; min-height: 240px; }
.capture-image img { display: block; max-width: 100%; max-height: 70svh; object-fit: contain; }
.player-controls { display: flex; flex-wrap: wrap; gap: 6px; padding: 8px; border-top: 1px solid var(--line); background: var(--panel); }
.fullscreen-button { margin-inline-start: auto; }
.capture-player:fullscreen, .capture-player.expanded { display: flex; flex-direction: column; width: 100%; height: 100%; margin: 0; border: 0; border-radius: 0; }
.capture-player.expanded { position: fixed; inset: 0; z-index: 100; height: 100dvh; }
.capture-player:fullscreen .capture-image, .expanded .capture-image { flex: 1; min-height: 0; overflow: hidden; background: #000; }
.capture-player:fullscreen img, .expanded img { width: 100%; height: 100%; max-height: 100%; min-height: 0; }
.capture-player:fullscreen .player-controls, .expanded .player-controls { flex-shrink: 0; padding-bottom: max(8px, env(safe-area-inset-bottom)); }
</style>
