import assert from 'node:assert/strict';
import { test } from 'node:test';
import { defaultScene, parseResize, ResizeQueue } from '../src/api/resize';
import type { ResizePatch } from '../src/api/resize';
import { loadProfiles, normalizeCard, saveProfiles } from '../src/storage';
import { useController } from '../src/useController';

test('resize queue coalesces edits and preserves changes arriving during an RPC', async () => {
  const sent: ResizePatch[] = [];
  let release!: () => void;
  const gate = new Promise<void>(resolve => { release = resolve; });
  const q = new ResizeQueue(async patch => { sent.push(patch); if (sent.length === 1) await gate; }, error => { throw error; });
  q.update({ scene: 1, offset_x: 1 }); q.update({ scene: 1, offset_y: -2 });
  const flush = q.flush();
  q.update({ scene: 1, scale_x: 1.2 });
  release(); await flush;
  assert.deepEqual(sent, [{ scene: 1, offset_x: 1, offset_y: -2 }, { scene: 1, scale_x: 1.2 }]);
  q.cancel(); q.update({ offset_x: 999 }); await q.flush(); assert.equal(sent.length, 2);
});

test('demo calibration, aspect ratio, save and scene changes share the production controller path', async () => {
  const c = useController();
  try {
    await c.startDemo();
    assert.equal(c.resizeError.value, ''); assert.equal(c.resize.value?.scenes.length, 4);
    c.updateResize({ scale_x: .97 }); c.updateResize({ offset_y: -4 });
    await c.selectScene(2);
    assert.equal(c.resize.value!.scenes[0]!.scale_x, .97);
    assert.equal(c.resize.value!.scenes[0]!.offset_y, -4);
    assert.equal(c.resize.value!.scene, 2);
    c.updateResize({ keep_aspect_ratio: true, scale_y: 1.02 });
    await c.saveResize();
    assert.equal(c.currentScene.value.scale_x, 1.02); assert.equal(c.currentScene.value.scale_y, 1.02);
    assert.equal(c.unsaved.value, false); assert.equal(c.error.value, '');
    await c.pulse('Service'); assert.equal(c.busy.value, '');
    await c.insertCoin(); assert.equal(c.coinQueue.value, 1);
  } finally { c.disconnect(); }
});

test('resize parser rejects incompatible state and accepts normal float precision', () => {
  const valid = { enabled: true, scene: 1, linear_filter: true, scenes: Array.from({ length: 4 }, defaultScene) };
  valid.scenes[0]!.scale_x = .60000002384;
  assert.equal(parseResize([valid]).scene, 1);
  assert.throws(() => parseResize([{ ...valid, scene: 0 }]));
  assert.throws(() => parseResize([{ ...valid, scenes: [] }]));
  assert.throws(() => parseResize([{ ...valid, enabled: 'yes' }]));
});

test('saving disabled calibration preserves its values and explicitly reports that it is inactive', async () => {
  const c = useController();
  try {
    await c.startDemo();
    c.updateResize({ enabled: false, scale_x: .9, offset_x: 12 });
    await c.saveResize();
    assert.equal(c.error.value, '');
    assert.equal(c.resize.value!.enabled, false);
    assert.equal(c.currentScene.value.scale_x, .9);
    assert.equal(c.currentScene.value.offset_x, 12);
    assert.match(c.message.value, /縮放尚未啟用/);
    c.updateResize({ enabled: true });
    await c.saveResize();
    assert.equal(c.resize.value!.enabled, true);
    assert.equal(c.currentScene.value.scale_x, .9);
    assert.match(c.message.value, /縮放已啟用/);
    assert.equal(c.unsaved.value, false);
  } finally { c.disconnect(); }
});

test('editing or selecting a scene enables a previously disabled transform', async () => {
  const c = useController();
  try {
    await c.startDemo();
    c.updateResize({ enabled: false }); await c.saveResize();
    c.updateResize({ offset_y: 13 }); await c.saveResize();
    assert.equal(c.resize.value!.enabled, true);
    assert.equal(c.currentScene.value.offset_y, 13);
    c.updateResize({ enabled: false }); await c.saveResize();
    await c.selectScene(3);
    assert.equal(c.resize.value!.enabled, true);
    assert.equal(c.resize.value!.scene, 3);
    assert.equal(c.unsaved.value, true);
  } finally { c.disconnect(); }
});

test('system actions in demo never disconnect or claim a real machine action', async () => {
  const c = useController();
  try {
    await c.startDemo();
    for (const command of ['restart', 'exit', 'reboot', 'shutdown'] as const) {
      await c.machineControl(command);
      assert.equal(c.error.value, '');
      assert.equal(c.connected.value, true);
      assert.match(c.message.value, /示範.*未送到機台/);
    }
  } finally { c.disconnect(); }
});

test('card queries from a previous connection are discarded', async () => {
  const c = useController();
  await c.startDemo();
  const pending = c.getMachineCards();
  c.disconnect();
  await assert.rejects(pending, /連線已變更/);
});

test('passwords are persisted only with an explicit remember choice', () => {
  let saved = '';
  const profiles = loadProfiles({ getItem: () => JSON.stringify([
    { id: 'private', name: 'Private', endpoint: 'ws://127.0.0.1:1338', password: '', rememberPassword: false },
    { id: 'remembered', name: 'Remembered', endpoint: 'ws://127.0.0.1:1339', password: '', rememberPassword: false },
  ]) });
  profiles[0]!.password = 'private'; profiles[1]!.password = 'remembered'; profiles[1]!.rememberPassword = true;
  saveProfiles(profiles, { setItem: (_key, value) => { saved = value; } });
  const stored = JSON.parse(saved) as Array<{ password: string }>;
  assert.equal(stored[0]!.password, '');
  assert.equal(stored[1]!.password, 'remembered');
  const restored = loadProfiles({ getItem: () => saved });
  assert.equal(restored[0]!.password, ''); assert.equal(restored[1]!.password, 'remembered');
  assert.equal(loadProfiles({ getItem: () => '{broken' }).length, 0);
  assert.equal(loadProfiles({ getItem: () => { throw new Error('storage blocked'); } }).length, 0);
});

test('card IDs are normalized and malformed values cannot be inserted', () => {
  assert.equal(normalizeCard('e004 0000 0000 0001'), 'E004000000000001');
  for (const value of ['', 'E004', 'G004000000000001', 'E0040000000000010']) assert.throws(() => normalizeCard(value));
});
