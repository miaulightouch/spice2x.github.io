<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue';
import { msg, t } from '../i18n';
import { readUpdateFile, UpdateInputError } from '../api/update';
import type { UpdateCandidate, UpdateResult, SignatureStatus } from '../api/update';
import { shouldReloadUpdateStatus, UpdateUploadCancelled, UpdateUploadCleanupError, uploadUpdate } from '../api/updateFlow';
import type { useController } from '../useController';

type Controller = ReturnType<typeof useController>;

const props = defineProps<{
  controller: Controller;
  active: boolean;
  connected: boolean;
  demo: boolean;
  configuredPassword: boolean;
}>();
defineEmits<{ connection: [] }>();

const c = props.controller;
const updateStatus = c.updateStatus;
const updateError = c.updateError;
const busy = c.busy;
const version = c.version;
const selected = ref<UpdateCandidate[]>();
const sourceName = ref('');
const uploadId = ref('');
const result = ref<UpdateResult>();
const phase = ref<'idle' | 'reading' | 'ready' | 'uploading' | 'finishing' | 'cancelling' | 'applying' | 'reconnecting' | 'done'>('idle');
const progress = ref({ sent: 0, total: 0, file: '' });
const localError = ref('');
let operation = 0;
let connectionEpoch = 0;
let reconnectTimer: ReturnType<typeof setTimeout> | undefined;
let uploadTask: Promise<void> | undefined;

const signatureLabels: Record<SignatureStatus, Parameters<typeof t>[0]> = {
  verified: 'update.signatureVerified', unsigned: 'update.signatureUnsigned', untrusted: 'update.signatureUntrusted',
  invalid: 'update.signatureInvalid', unsupported: 'update.signatureUnsupported',
};

function fileError(value: unknown): string {
  if (value instanceof UpdateInputError) return t(value.reason === 'unsupported' ? 'error.updateInvalidSelection' : 'error.updateInvalidFile');
  return value instanceof Error ? value.message : String(value);
}

function formatBytes(value: number): string {
  if (!Number.isFinite(value) || value < 0) return '—';
  const units = ['B', 'KiB', 'MiB', 'GiB'];
  let amount = value; let unit = 0;
  while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit++; }
  return `${amount.toFixed(unit ? 1 : 0)} ${units[unit]}`;
}

function resetLocal(): void {
  operation++;
  selected.value = undefined; sourceName.value = ''; uploadId.value = ''; result.value = undefined;
  phase.value = 'idle'; progress.value = { sent: 0, total: 0, file: '' }; localError.value = '';
}

async function loadStatus(reset = true): Promise<boolean> {
  if (reset && uploadId.value && props.connected && !(await cancel())) return false;
  if (reset) resetLocal();
  if (props.connected && props.configuredPassword && !props.demo) {
    await c.readUpdateStatus();
    return !!c.updateStatus.value;
  }
  return false;
}

watch(() => ({ active: props.active, connected: props.connected, password: props.configuredPassword, demo: props.demo }), (current, previous) => {
  if (previous && previous.connected !== current.connected) connectionEpoch++;
  if (!current.connected || !current.password || current.demo) {
    if (phase.value === 'applying' || phase.value === 'reconnecting') { uploadId.value = ''; return; }
    if (uploadId.value) {
      operation++; uploadId.value = ''; result.value = undefined; progress.value = { sent: 0, total: 0, file: '' };
      if (selected.value?.length) phase.value = 'ready'; else phase.value = 'idle';
      return;
    }
    resetLocal();
    return;
  }
  if (shouldReloadUpdateStatus(current, previous, !!updateStatus.value, phase.value)) void loadStatus(false);
}, { immediate: true });

function inputFile(event: Event): void {
  const file = (event.target as HTMLInputElement).files?.[0];
  if (!file || !c.updateStatus.value || phase.value === 'uploading' || phase.value === 'finishing' || phase.value === 'applying') return;
  const token = ++operation;
  selected.value = undefined; sourceName.value = file.name; result.value = undefined; uploadId.value = ''; localError.value = '';
  phase.value = 'reading'; progress.value = { sent: 0, total: 0, file: '' };
  void readUpdateFile(file, c.updateStatus.value).then(files => {
    if (token !== operation) return;
    selected.value = files; phase.value = 'ready';
  }).catch(error => {
    if (token !== operation) return;
    phase.value = 'idle'; localError.value = fileError(error); selected.value = undefined;
  });
}

async function upload(): Promise<void> {
  if (!selected.value?.length || phase.value !== 'ready' || uploadId.value) return;
  const files = selected.value;
  const token = ++operation;
  const uploadEpoch = connectionEpoch;
  localError.value = ''; result.value = undefined; progress.value = { sent: 0, total: files.reduce((sum, file) => sum + file.size, 0), file: '' };
  phase.value = 'uploading';
  try {
    const finished = await uploadUpdate(files, c.updateStatus.value?.chunk_bytes ?? 32768, {
      begin: entries => c.beginUpdate(entries), chunk: (id, name, offset, data) => c.updateChunk(id, name, offset, data),
      finish: id => c.finishUpdate(id), cancel: id => c.cancelUpdate(id),
    }, {
      cancelled: () => token !== operation,
      started: id => { if (token === operation) uploadId.value = id; },
      finishing: () => { if (token === operation) phase.value = 'finishing'; },
      progress: (sent, file) => { if (token === operation) progress.value = { ...progress.value, sent, file }; },
    });
    if (token !== operation) return;
    if (!finished) { phase.value = selected.value?.length ? 'ready' : 'idle'; return; }
    result.value = finished; phase.value = 'ready';
  } catch (error) {
    if (error instanceof UpdateUploadCleanupError) {
      if (uploadEpoch !== connectionEpoch) return;
      uploadId.value = error.id; phase.value = 'ready'; localError.value = t('error.updateCleanup');
      return;
    }
    if (token !== operation) {
      if (error instanceof UpdateUploadCancelled) {
        phase.value = selected.value?.length ? 'ready' : 'idle'; uploadId.value = ''; result.value = undefined; progress.value = { sent: 0, total: 0, file: '' }; localError.value = '';
      }
      return;
    }
    localError.value = error instanceof Error ? error.message : String(error); phase.value = selected.value?.length ? 'ready' : 'idle'; uploadId.value = '';
  }
}

function startUpload(): void {
  const task = upload();
  uploadTask = task;
  void task.then(() => { if (uploadTask === task) uploadTask = undefined; }, () => { if (uploadTask === task) uploadTask = undefined; });
}

async function cancel(): Promise<boolean> {
  const id = uploadId.value;
  const active = phase.value === 'uploading' || phase.value === 'finishing';
  operation++;
  if (!id) {
    phase.value = selected.value?.length ? 'ready' : 'idle'; result.value = undefined; progress.value = { sent: 0, total: 0, file: '' }; localError.value = '';
    return true;
  }
  if (active && uploadTask) {
    phase.value = 'cancelling';
    await uploadTask;
    if (!uploadId.value) {
      phase.value = selected.value?.length ? 'ready' : 'idle'; localError.value = '';
      return true;
    }
    return false;
  }
  if (props.connected) {
    while (c.busy.value && props.connected) await new Promise(resolve => setTimeout(resolve, 20));
    if (props.connected && await c.cancelUpdate(id)) {
      phase.value = selected.value?.length ? 'ready' : 'idle'; uploadId.value = ''; result.value = undefined; progress.value = { sent: 0, total: 0, file: '' }; localError.value = '';
      return true;
    }
  }
  phase.value = 'ready'; localError.value = t('error.updateCleanup'); return false;
}

async function apply(): Promise<void> {
  const id = uploadId.value;
  if (!id || phase.value !== 'ready') return;
  const main = selected.value?.find(file => file.name.toLowerCase() === c.updateStatus.value?.executable.toLowerCase());
  if (!main) {
    localError.value = t('error.updateVerify'); return;
  }
  const token = ++operation;
  localError.value = ''; phase.value = 'applying';
  const accepted = await c.applyUpdate(id);
  if (token !== operation) return;
  // A close before the response leaves the result uncertain. Reconnect only to
  // verify the fresh status; the apply request itself is never replayed.
  if (!accepted && c.status.value === 'connected') {
    phase.value = 'ready'; localError.value = t('error.updateApply'); return;
  }
  phase.value = 'reconnecting';
  const deadline = Date.now() + 40000;
  let delay = 1200;
  while (Date.now() < deadline) {
    await new Promise<void>(resolve => { reconnectTimer = setTimeout(resolve, delay); });
    if (token !== operation) return;
    await c.reconnect();
    if (token !== operation) return;
    if (c.status.value === 'connected' && c.version.value) {
      const refreshed = await c.readUpdateStatus();
      if (token !== operation) return;
      if (refreshed?.executable_crc32 === main.crc32 && refreshed.update_id === id) {
        phase.value = 'done'; uploadId.value = ''; c.notify(msg('msg.updateApplied', { version: c.version.value })); return;
      }
    }
    delay = Math.min(4000, delay + 800);
  }
  phase.value = 'ready'; localError.value = t('error.updateReconnect');
}

function signatureLabel(status: SignatureStatus): string { return t(signatureLabels[status]); }

onBeforeUnmount(() => {
  operation++;
  if (reconnectTimer) clearTimeout(reconnectTimer);
  if (uploadId.value && props.connected && phase.value !== 'uploading' && phase.value !== 'finishing') void c.cancelUpdate(uploadId.value);
});
</script>

<template>
  <div class="system-update">
    <div class="page-title"><h1 id="update-title">{{ t('update.title') }}</h1><button class="quiet compact" :disabled="!connected || !configuredPassword || demo || !!busy" @click="loadStatus()">{{ t('update.refresh') }}</button></div>
    <div v-if="!connected" class="empty-state"><h2>{{ t('update.connect') }}</h2><p>{{ t('update.connectHint') }}</p></div>
    <div v-else-if="demo" class="empty-state"><h2>{{ t('update.unavailable') }}</h2><p>{{ t('error.updateDemo') }}</p></div>
    <div v-else-if="!configuredPassword" class="empty-state"><h2>{{ t('update.passwordRequired') }}</h2><p>{{ t('error.updatePassword') }}</p><button @click="$emit('connection')">{{ t('update.connectionSettings') }}</button></div>
    <div v-else-if="updateError" class="empty-state"><h2>{{ t('update.unavailable') }}</h2><p>{{ updateError }}</p><button :disabled="!!busy" @click="loadStatus()">{{ t('update.retry') }}</button></div>
    <template v-else-if="updateStatus">
      <div class="feature-cards update-cards">
      <div class="settings-panel update-summary">
        <div class="section-toolbar"><h2>{{ t('update.machine') }}</h2><span class="muted small">{{ updateStatus.architecture }}{{ updateStatus.xp ? ' · XP' : '' }}</span></div>
        <dl class="status-list"><div><dt>{{ t('status.version') }}</dt><dd>{{ updateStatus.version || '—' }}</dd></div><div><dt>{{ t('update.executable') }}</dt><dd><code>{{ updateStatus.executable }}</code></dd></div></dl>
      </div>
      <div class="settings-panel update-upload">
        <h2>{{ t('update.selectTitle') }}</h2><p class="muted small">{{ t('update.selectHint') }}</p>
        <label class="stacked-field">{{ t('update.file') }}<input type="file" accept=".zip,.exe,application/zip,application/vnd.microsoft.portable-executable" :disabled="!!uploadId || phase === 'reading' || phase === 'uploading' || phase === 'finishing' || phase === 'applying'" @change="inputFile"></label>
        <p v-if="sourceName && phase === 'reading'" class="muted small">{{ t('update.reading') }}</p>
        <p v-if="localError" class="error-text" role="alert">{{ localError }}</p>
        <div v-if="selected?.length" class="update-files"><strong>{{ t('update.selectedFiles') }}</strong><div v-for="file in selected" :key="file.name" class="update-file"><code>{{ file.name }}</code><span class="muted small">{{ formatBytes(file.size) }}</span></div></div>
        <div v-if="phase === 'uploading' || phase === 'finishing'" class="update-progress" role="status"><div class="field-header"><span>{{ phase === 'finishing' ? t('update.finishing') : t('update.uploading') }}</span><span class="numeric">{{ formatBytes(progress.sent) }} / {{ formatBytes(progress.total) }}</span></div><progress :max="progress.total || 1" :value="progress.sent"></progress><p class="muted small">{{ progress.file }}</p></div>
        <div class="panel-actions"><button :disabled="phase !== 'ready' || !!uploadId || !!busy" @click="startUpload">{{ t('update.upload') }}</button><button v-if="uploadId && phase !== 'finishing' && phase !== 'applying' && phase !== 'reconnecting' && phase !== 'done'" class="quiet" :disabled="phase === 'cancelling'" @click="cancel">{{ t('update.cancel') }}</button></div>
      </div>
      <div v-if="result" class="settings-panel update-result"><h2>{{ t('update.signatures') }}</h2><div v-for="file in result.files" :key="file.name" class="update-file"><div><code>{{ file.name }}</code><span v-if="file.signature.signer" class="muted small"> · {{ file.signature.signer }}</span><p v-if="file.signature.detail" class="muted small">{{ file.signature.detail }}</p></div><span class="signature-status" :data-status="file.signature.status">{{ signatureLabel(file.signature.status) }}</span></div><p class="muted small">{{ t('update.signatureHint') }}</p></div>
      <div v-if="result && phase === 'ready'" class="settings-panel update-apply"><h2>{{ t('update.applyTitle') }}</h2><p class="muted small">{{ t('update.applyHint') }}</p><button class="primary" :disabled="!uploadId || !!busy" @click="apply">{{ t('update.apply') }}</button></div>
      </div>
      <div v-if="phase === 'applying' || phase === 'reconnecting'" class="empty-state"><h2>{{ t('update.restarting') }}</h2><p>{{ phase === 'applying' ? t('update.restartingHint') : t('update.reconnectHint') }}</p></div>
      <div v-if="phase === 'done'" class="empty-state"><h2>{{ t('update.complete') }}</h2><p>{{ t('msg.updateApplied', { version }) }}</p></div>
    </template>
  </div>
</template>
