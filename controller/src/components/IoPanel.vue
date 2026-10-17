<script setup lang="ts">
import { reactive, ref, watch } from 'vue';
import { t } from '../i18n';
import { DEFAULT_IO_PULSE_MS, MAX_IO_PULSE_MS, MIN_IO_PULSE_MS, validateIoPulse } from '../api/io';

export interface IoPanelItem {
  name: string;
  value: number;
  overridden: boolean;
}

const props = defineProps<{
  title: string;
  kind: 'button' | 'analog' | 'light';
  items: IoPanelItem[];
  disabled: boolean;
}>();

const emit = defineEmits<{
  test: [name: string, value: number, duration: number];
  reset: [name?: string];
}>();

const duration = ref(DEFAULT_IO_PULSE_MS);
const draftValues = reactive(new Map<string, number>());
const activeDrafts = new Set<string>();
const adjustable = () => props.kind === 'analog' || props.kind === 'light';

function unitValue(value: number): number {
  return Number.isFinite(value) ? Math.min(1, Math.max(0, value)) : 0;
}

function shownValue(item: IoPanelItem): string {
  return item.value.toFixed(props.kind === 'button' ? 2 : 3);
}

function draftValue(item: IoPanelItem): number {
  return draftValues.get(item.name) ?? unitValue(item.value);
}

watch(() => props.items, items => {
  const names = new Set(items.map(item => item.name));
  for (const name of draftValues.keys()) {
    if (!names.has(name)) { draftValues.delete(name); activeDrafts.delete(name); }
  }
  for (const item of items) {
    if (!activeDrafts.has(item.name)) draftValues.set(item.name, unitValue(item.value));
  }
}, { deep: true, immediate: true });

function setDuration(event: Event): void {
  const raw = Number((event.target as HTMLInputElement).value);
  duration.value = Number.isFinite(raw) ? Math.trunc(raw) : DEFAULT_IO_PULSE_MS;
}

function setValue(name: string, event: Event): void {
  const raw = Number((event.target as HTMLInputElement).value);
  if (!Number.isFinite(raw)) return;
  activeDrafts.add(name);
  draftValues.set(name, unitValue(raw));
}

function canTest(item: IoPanelItem): boolean {
  const value = props.kind === 'button' ? 1 : draftValue(item);
  return validateIoPulse(value, duration.value);
}

function testItem(item: IoPanelItem): void {
  const value = props.kind === 'button' ? 1 : draftValue(item);
  if (!validateIoPulse(value, duration.value)) return;
  emit('test', item.name, value, duration.value);
  activeDrafts.delete(item.name);
}

function resetItem(name: string): void {
  activeDrafts.delete(name);
  draftValues.delete(name);
  emit('reset', name);
}

function resetGroup(): void {
  activeDrafts.clear();
  draftValues.clear();
  emit('reset');
}
</script>

<template>
  <div class="settings-panel io-panel">
    <div class="section-toolbar">
      <h2>{{ title }}</h2>
      <div class="button-row">
        <label class="inline-field io-duration">
          <span>{{ t('io.duration') }}</span>
          <input
            type="number"
            :min="MIN_IO_PULSE_MS"
            :max="MAX_IO_PULSE_MS"
            step="1"
            :value="duration"
            :aria-label="t('io.duration')"
            :disabled="disabled"
            @input="setDuration"
          >
        </label>
        <button
          type="button"
          class="quiet compact"
          :disabled="disabled"
          :aria-label="`${t('io.resetGroup')} ${title}`"
          @click="resetGroup"
        >{{ t('io.resetGroup') }}</button>
      </div>
    </div>
    <div v-if="!items.length" class="muted small">{{ t('io.empty') }}</div>
    <div v-for="item in items" :key="item.name" class="io-row" :class="{ 'io-row--adjustable': adjustable() }">
      <span class="io-name">{{ item.name }}</span>
      <output class="io-live-value">{{ shownValue(item) }}</output>
      <div v-if="adjustable()" class="io-value-control">
        <label class="sr-only" :for="`io-value-${kind}-${item.name}`">{{ t('io.value') }} {{ item.name }}</label>
        <input
          :id="`io-value-${kind}-${item.name}`"
          type="range"
          min="0"
          max="1"
          step="0.01"
          :value="draftValue(item)"
          :aria-label="`${t('io.value')} ${item.name}`"
          :disabled="disabled"
          @input="setValue(item.name, $event)"
        >
        <output class="io-draft-value">{{ draftValue(item).toFixed(3) }}</output>
      </div>
      <button
        type="button"
        class="compact io-test"
        :disabled="disabled || !canTest(item)"
        :aria-label="`${t('io.test')} ${item.name}`"
        @click="testItem(item)"
      >{{ t('io.test') }}</button>
      <button
        type="button"
        class="quiet compact io-reset"
        :disabled="disabled"
        :aria-label="`${t('io.resetItem')} ${item.name}`"
        @click="resetItem(item.name)"
      >{{ t('io.resetItem') }}</button>
      <span v-if="item.overridden" class="muted small io-override">{{ t('io.override') }}</span>
    </div>
  </div>
</template>

<style scoped>
.io-panel { min-width: 0; }
.io-duration { white-space: nowrap; }
.io-duration input { width: 6.5rem; }
.io-row { grid-template-columns: minmax(0,1fr) auto auto auto; }
.io-row--adjustable { grid-template-columns: minmax(0,1fr) auto auto auto; }
.io-name { min-width: 0; overflow-wrap: anywhere; }
.io-test { grid-column: 3; grid-row: 1; }
.io-reset { grid-column: 4; grid-row: 1; }
.io-live-value,.io-draft-value { font-variant-numeric: tabular-nums; color: var(--muted); white-space: nowrap; }
.io-value-control { grid-column: 1 / -1; grid-row: 2; min-width: 0; display: grid; grid-template-columns: minmax(5rem,1fr) auto; align-items: center; gap: 6px; }
.io-value-control input[type=range] { min-width: 5rem; }
.io-override { grid-column: 1 / -1; }
@media(max-width:520px) {
  .io-duration { width: 100%; justify-content: space-between; }
  .io-row,.io-row--adjustable { grid-template-columns: minmax(0,1fr) auto; }
  .io-live-value { grid-column: 2; grid-row: 1; }
  .io-value-control { grid-column: 1 / -1; grid-row: 2; }
  .io-test { grid-column: 1; grid-row: 3; }
  .io-reset { grid-column: 2; grid-row: 3; }
  .io-row:not(.io-row--adjustable) .io-test { grid-row: 2; }
  .io-row:not(.io-row--adjustable) .io-reset { grid-row: 2; }
  .io-override { grid-row: 4; }
  .io-row:not(.io-row--adjustable) .io-override { grid-row: 3; }
}
</style>
