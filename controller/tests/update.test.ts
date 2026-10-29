import assert from 'node:assert/strict';
import { test } from 'node:test';
import { zipSync } from 'fflate';
import {
  encodeBase64, matchesUpdateResult, parseUpdateApply, parseUpdateFinish, parseUpdateStatus, selectUpdateFiles, UpdateInputError,
} from '../src/api/update';
import type { UpdateStatus } from '../src/api/update';
import { reactive, nextTick, watch } from 'vue';
import { shouldReloadUpdateStatus, uploadUpdate, UpdateUploadCancelled, UpdateUploadCleanupError } from '../src/api/updateFlow';

const status = (changes: Partial<UpdateStatus> = {}): UpdateStatus => ({
  version: '2026-10-29', architecture: 'x64', xp: false, executable: 'spice64.exe',
  executable_crc32: 0, update_id: '',
  max_file_bytes: 64 * 1024 * 1024, max_total_bytes: 128 * 1024 * 1024, chunk_bytes: 32768, ...changes,
});

test('update status and finish parsers keep the native contract and advisory signature states', () => {
  assert.deepEqual(parseUpdateStatus([{ ...status(), max_file_bytes: 64, max_total_bytes: 128, chunk_bytes: 8 }]), { ...status(), max_file_bytes: 64, max_total_bytes: 128, chunk_bytes: 8 });
  const result = parseUpdateFinish([{ files: [
    { name: 'spice64.exe', size: 3, signature: { status: 'verified', signer: 'spice2x', detail: '' } },
    { name: 'spicecfg.exe', size: 2, signature: { status: 'invalid', signer: '', detail: 'test only' } },
  ] }]);
  assert.equal(result.files[1]!.signature.status, 'invalid');
  assert.equal(parseUpdateApply([{ restarting: true }]), true);
  assert.equal(parseUpdateStatus([{ ...status(), update_id: undefined }]).update_id, '');
  assert.throws(() => parseUpdateStatus([{ ...status(), update_id: 123 }]), /Invalid update API response/);
  assert.throws(() => parseUpdateStatus([{ ...status(), architecture: 'arm64' }]), /Invalid update API response/);
});

test('ZIP selection extracts only the current architecture executables', () => {
  const files = zipSync({
    'spice64.exe': new Uint8Array([1, 2, 3]),
    'spice.exe': new Uint8Array([4]),
    'spicecfg.exe': new Uint8Array([5, 6]),
    'src/ignored.cpp': new Uint8Array([7, 8, 9]),
  });
  const selected = selectUpdateFiles('spice2x-full.zip', files, status());
  assert.deepEqual(selected.map(file => [file.name, file.source, file.size]), [
    ['spice64.exe', 'spice64.exe', 3], ['spicecfg.exe', 'spicecfg.exe', 2],
  ]);
  assert.equal(selected[0]!.crc32, 1438416925);
  const duplicate = zipSync({ 'spice64.exe': new Uint8Array([1]), 'SPICE64.EXE': new Uint8Array([2]) });
  assert.throws(() => selectUpdateFiles('duplicate.zip', duplicate, status()), UpdateInputError);
});

test('XP selection requires extras/winxp and never falls back to root binaries', () => {
  const xp = status({ architecture: 'x86', executable: 'spice.exe', xp: true, max_file_bytes: 16, max_total_bytes: 1024 });
  const rootOnly = zipSync({ 'spice.exe': new Uint8Array([1]), 'spicecfg.exe': new Uint8Array([2]) });
  assert.throws(() => selectUpdateFiles('full.zip', rootOnly, xp), (error: unknown) => error instanceof UpdateInputError && error.reason === 'invalidfile');
  const full = zipSync({
    'spice.exe': new Uint8Array([8]),
    'extras/winxp/spice.exe': new Uint8Array([3, 4]),
    'extras/winxp/spicecfg.exe': new Uint8Array([5]),
  });
  const selected = selectUpdateFiles('full.zip', full, xp);
  assert.deepEqual(selected.map(file => file.source), ['extras/winxp/spice.exe', 'extras/winxp/spicecfg.exe']);
});

test('raw files, sidecars, and archive bombs are bounded before upload', () => {
  const small = status({ max_file_bytes: 4, max_total_bytes: 8 });
  assert.equal(selectUpdateFiles('spice64.exe', new Uint8Array([1, 2]), small)[0]!.name, 'spice64.exe');
  assert.throws(() => selectUpdateFiles('SPICECFG.EXE', new Uint8Array([1]), small), UpdateInputError);
  for (const name of ['release.sig', 'notes.txt']) assert.throws(() => selectUpdateFiles(name, new Uint8Array([1]), small), UpdateInputError);
  const bomb = zipSync({ 'spice64.exe': new Uint8Array(9) });
  assert.throws(() => selectUpdateFiles('release.zip', bomb, small), UpdateInputError);
  const configOnly = zipSync({ 'spicecfg.exe': new Uint8Array([1]) });
  assert.throws(() => selectUpdateFiles('config-only.zip', configOnly, small), UpdateInputError);
  assert.throws(() => selectUpdateFiles('spice64.exe', new Uint8Array(9), small), UpdateInputError);
  assert.equal(encodeBase64(new Uint8Array([0, 1, 2, 253, 254, 255])), 'AAEC/f7/');
  const files = [{ name: 'spice64.exe', size: 2, crc32: 0 }];
  const finished = parseUpdateFinish([{ files: [{ name: 'spice64.exe', size: 2, signature: { status: 'unsigned' } }] }]);
  assert.equal(matchesUpdateResult(finished, files), true);
  assert.equal(matchesUpdateResult({ files: [{ ...finished.files[0]!, name: 'spicecfg.exe' }] }, files), false);
});

function candidate(name: string, bytes: number[]): { name: string; source: string; size: number; crc32: number; data: Uint8Array } {
  return { name, source: name, size: bytes.length, crc32: 0, data: new Uint8Array(bytes) };
}

test('upload flow cancels an in-flight session and cleans up after stale begin or finish results', async () => {
  const file = candidate('spice64.exe', [1, 2, 3, 4]);
  const calls: string[] = [];
  let cancelled = false;
  const transport = {
    async begin() { calls.push('begin'); return 'upload-1'; },
    async chunk(_id: string, _name: string, offset: number) { calls.push(`chunk:${offset}`); return offset + 2; },
    async finish() { calls.push('finish'); return { files: [{ name: file.name, size: file.size, signature: { status: 'unsigned' as const, signer: '', detail: '' } }] }; },
    async cancel(id: string) { calls.push(`cancel:${id}`); cancelled = true; return true; },
  };
  let stop = false;
  await assert.rejects(uploadUpdate([file], 2, transport, { cancelled: () => stop, started: () => { stop = true; } }), UpdateUploadCancelled);
  assert.equal(cancelled, true);
  assert.deepEqual(calls, ['begin', 'cancel:upload-1']);

  const broken = {
    ...transport,
    async begin() { calls.push('begin-broken'); return 'upload-2'; },
    async finish() { calls.push('finish-broken'); return { files: [] }; },
    async cancel(id: string) { calls.push(`cancel:${id}`); return true; },
  };
  await assert.rejects(uploadUpdate([file], 2, broken), /Update upload failed/);
  assert.deepEqual(calls.slice(-5), ['begin-broken', 'chunk:0', 'chunk:2', 'finish-broken', 'cancel:upload-2']);
  assert.equal(calls.at(-1), 'cancel:upload-2');
});

test('failed cleanup preserves the upload ID and reactive reconnect reloads status', async () => {
  const file = candidate('spice64.exe', [1, 2]);
  const cleanupFailure = {
    async begin() { return 'stuck-upload'; },
    async chunk() { return 0; },
    async finish() { return undefined; },
    async cancel() { return false; },
  };
  await assert.rejects(uploadUpdate([file], 2, cleanupFailure), (error: unknown) => error instanceof UpdateUploadCleanupError && error.id === 'stuck-upload');

  const state = reactive({ active: true, connected: false, password: true, demo: false });
  let reloads = 0;
  watch(state, (current, previous) => {
    if (shouldReloadUpdateStatus(current, previous, false, 'ready')) reloads++;
  }, { immediate: true });
  state.connected = true;
  await nextTick();
  assert.equal(reloads, 1);
});
