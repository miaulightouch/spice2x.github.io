import assert from 'node:assert/strict';
import { test } from 'node:test';
import { DemoApi } from '../src/api/demo';
import type { Rpc } from '../src/api/client';
import { groupPatches, parseCardManager, parseKeypadCapabilities, parseOptionChoices, parseOptions, parsePatches } from '../src/api/features';
import type { OptionField } from '../src/api/features';
import { validOptionValue } from '../src/api/options';
import { mjpegUrl, parseCaptureStreams, readCaptureFrame } from '../src/api/capture';
import { useController } from '../src/useController';

const delay = (ms: number) => new Promise<void>(resolve => setTimeout(resolve, ms));

class ParityRpc extends DemoApi implements Rpc {
  calls: Array<{ module: string; fn: string; params: unknown[] }> = [];
  failActive = false;
  logDelay = 0;
  logFailure = false;
  logReads = 0;
  activeLogs = 0;
  maxLogs = 0;
  async request(module: string, fn: string, params: unknown[] = []): Promise<unknown[]> {
    this.calls.push({ module, fn, params });
    if (module === 'card' && fn === 'manager_set_active' && this.failActive) throw new Error('active failed');
    if (module === 'info' && fn === 'log') {
      this.logReads++; this.activeLogs++; this.maxLogs = Math.max(this.maxLogs, this.activeLogs);
      try {
        await delay(this.logDelay);
        if (this.logFailure) throw new Error('log failed');
        return [{ offset: 0, next: 4, size: 4, data: 'bG9nCg==' }];
      } finally { this.activeLogs--; }
    }
    return super.request(module, fn, params);
  }
}

test('option validation matches native decimal, hex and empty-default boundaries', () => {
  const option = (type: OptionField['type']): OptionField => ({ name: 'test', title: '', description: '', category: '', type,
    value: '', sensitive: false, has_value: false, hidden: false, disabled: false, elements: [{ value: 'a', label: 'A' }] });
  for (const type of ['text', 'integer', 'hex', 'enum', 'bool'] as const) assert.equal(validOptionValue(option(type), ''), true);
  assert.equal(validOptionValue(option('integer'), '4294967295'), true);
  for (const value of ['-1', '1.1', '4294967296', '1e2', ' 1']) assert.equal(validOptionValue(option('integer'), value), false);
  for (const value of ['FFFFFFFFFFFFFFFF', '0xFFFFFFFFFFFFFFFF', '0X01']) assert.equal(validOptionValue(option('hex'), value), true);
  for (const value of ['0x', '10000000000000000', '-1', 'FG']) assert.equal(validOptionValue(option('hex'), value), false);
  assert.equal(validOptionValue(option('bool'), '/ENABLED'), true);
  assert.equal(validOptionValue(option('bool'), 'true'), false);
  assert.equal(validOptionValue(option('enum'), 'b'), false);
  assert.equal(validOptionValue(option('text'), 'abc\0def'), false);
  assert.equal(validOptionValue(option('text'), 'x'.repeat(4097)), false);
});

test('new metadata accepts older hosts and rejects malformed picker/card/capability payloads', async () => {
  const demo = new DemoApi();
  const options = parseOptions(await demo.request('config', 'get_options'));
  assert.deepEqual(options.quick_categories, ['Common']);
  assert.equal(options.options[0]?.quick_setting_category, 'Common');
  assert.deepEqual(parseOptionChoices([{ name: 'cpu', choices: [{ value: '0x1', label: 'CPU 0' }] }], 'cpu', 'cpu_affinity'), [{ value: '0x1', label: 'CPU 0' }]);
  assert.throws(() => parseOptionChoices([{ name: 'cpu', choices: [{ value: 'unsafe', label: 'CPU 0' }] }], 'cpu', 'cpu_affinity'));
  assert.throws(() => parseOptionChoices([{ name: 'other', choices: [] }], 'cpu'));
  assert.deepEqual(parseKeypadCapabilities([{ model: 'A', keypad_count: 1, keys: false, insert_card: true }]), { model: 'A', keypad_count: 1, keys: false, insert_card: true });
  for (const count of [-1, 1.5, 3, '2']) assert.throws(() => parseKeypadCapabilities([{ model: 'A', keypad_count: count, keys: true, insert_card: true }]));
  assert.deepEqual(parseCardManager([{ revision: 0, cards: [], active: ['', ''] }]).read_only, []);
  assert.throws(() => parseCardManager([{ revision: 0, cards: [], active: ['', ''], read_only: [{ name: 'P1', id: 'bad', source: '-card0' }] }]));
  const patches = parsePatches(await demo.request('patches', 'get'));
  assert.equal(patches.identity?.game_identifier, 'DEMO');
  assert.equal(patches.groups?.[0]?.name, 'Demo');
  assert.equal(patches.groups?.[0]?.game_code, 'DEMO');
});

test('ordinary actions preserve native parameters and never insert after a failed card load', async () => {
  const api = new ParityRpc(), c = useController(() => api);
  try {
    await c.startDemo();
    assert.equal(c.keypadCapabilities.value?.keypad_count, 2);
    await c.insertKeypadCard(1);
    await c.testIo('analog', 'Demo knob', 0.25, 75);
    await c.testIo('light', 'Demo lamp', 0.75, 200);
    await c.resetIo('light', 'Demo lamp');
    assert.ok(api.calls.some(call => call.module === 'keypads' && call.fn === 'insert_card' && call.params[0] === 1));
    assert.ok(api.calls.some(call => call.fn === 'pulse' && JSON.stringify(call.params) === '["Demo knob",0.25,75]'));
    assert.ok(api.calls.some(call => call.fn === 'write_reset' && JSON.stringify(call.params) === '[["Demo lamp"]]'));
    const before = api.calls.filter(call => call.fn === 'pulse').length;
    await c.testIo('analog', 'Demo knob', 1.5, 75);
    assert.equal(api.calls.filter(call => call.fn === 'pulse').length, before);
    await c.refreshCardManager();
    const id = c.cardManager.value!.cards[0]!.id;
    await c.loadAndInsertCard(1, id);
    assert.deepEqual(api.calls.filter(call => call.module === 'card').slice(-2).map(call => [call.fn, call.params]), [['manager_set_active', [1, id]], ['insert', [1, id]]]);
    api.failActive = true;
    const inserts = api.calls.filter(call => call.fn === 'insert').length;
    await c.loadAndInsertCard(0, id);
    assert.equal(api.calls.filter(call => call.fn === 'insert').length, inserts);
    assert.match(c.error.value, /active failed/);
  } finally { c.disconnect(); }
});

test('patch group, disable-all and history management share production actions', async () => {
  const c = useController();
  try {
    await c.startDemo(); await c.refreshPatches();
    await c.patchOperation('set_group', ['DEMO', 'Demo', true]);
    assert.equal(c.patches.value?.patches[0]?.enabled, true);
    await c.patchOperation('disable_all');
    assert.equal(c.patches.value?.patches[0]?.enabled, false);
    assert.equal(c.patches.value?.auto_apply, false);
    await c.importPatches('https://patch.example/');
    await c.importPatches('https://other.example/');
    await c.patchOperation('remove_recent_url', ['https://patch.example/']);
    assert.deepEqual(c.patches.value?.recent_urls, ['https://other.example/']);
    await c.patchOperation('clear_recent_urls');
    assert.deepEqual(c.patches.value?.recent_urls, []);
  } finally { c.disconnect(); }
});

test('patch grouping keeps game/id identities and full-group status while searching', async () => {
  const state = parsePatches(await new DemoApi().request('patches', 'get'));
  const patch = state.patches[0]!;
  state.groups = [{ game_code: 'A', id: 'first', name: 'Audio tweaks', description: '', caution: '' },
    { game_code: 'B', id: 'first', name: 'Audio tweaks', description: '', caution: '' }];
  state.patches = [{ ...patch, id: 'p1', name: 'Zed', game_code: 'A', group: 'first', status: 'enabled' },
    { ...patch, id: 'p2', name: 'Alpha', game_code: 'A', group: 'first', status: 'disabled' },
    { ...patch, id: 'p3', name: 'Other', game_code: 'B', group: 'first', status: 'error' }];
  const groups = groupPatches(state, 'audio', 'name');
  assert.equal(groups.length, 2);
  assert.equal(groups[0]!.metadata?.game_code, 'A');
  assert.deepEqual(groups[0]!.items.map(p => p.name), ['Alpha', 'Zed']);
  assert.equal(groups[0]!.mixed, true);
  assert.equal(groups[1]!.hasError, true);
  const filtered = groupPatches(state, 'Zed', 'name');
  assert.equal(filtered[0]!.items.length, 1);
  assert.equal(filtered[0]!.checked, false);
  assert.equal(filtered[0]!.mixed, true);
});

test('log polling drops a stopped response, avoids overlapping reads and stops after errors', async () => {
  const api = new ParityRpc(), c = useController(() => api);
  try {
    await c.startDemo(); api.logDelay = 30;
    c.startLogPolling(); c.stopLogPolling(); await delay(50);
    assert.equal(c.logText.value, '');
    api.logDelay = 0; c.startLogPolling(); await delay(40);
    assert.equal(c.logText.value, 'log\n');
    api.logFailure = true; await delay(1060);
    assert.equal(c.logPolling.value, false);
    assert.equal(api.maxLogs, 1);
    const reads = api.logReads; await delay(1050); assert.equal(api.logReads, reads);
  } finally { c.disconnect(); }
});

test('MJPEG URLs stay on the connected host and reject busy, unsupported and malformed streams', async () => {
  assert.equal(parseCaptureStreams([]), undefined);
  const streams = parseCaptureStreams([{ port: 1340, formats: [{ name: 'mjpeg', path: '/stream.mjpg' }], screens: [{ screen: 1, width: 1080, height: 1920, busy: false }] }])!;
  assert.equal(mjpegUrl('wss://cabinet.example/api?secret=discard', streams, 1, 80, 30), 'http://cabinet.example:1340/stream.mjpg?screen=1&q=80&fps=30');
  assert.throws(() => mjpegUrl('ws://cabinet.example', { ...streams, screens: [{ ...streams.screens[0]!, busy: true }] }, 1, 80, 30));
  assert.throws(() => mjpegUrl('ws://cabinet.example', { ...streams, formats: [{ name: 'mjpeg', path: '//evil.example/stream.mjpg' }] }, 1, 80, 30));
  for (const port of [0, 65536, '1340']) assert.throws(() => parseCaptureStreams([{ ...streams, port }]));
  const calls: unknown[][] = [];
  await readCaptureFrame({ request: async (_m, _f, params) => { calls.push(params!); return [1, 100, 200, 'YWJj']; } }, 1, 90, 1);
  assert.deepEqual(calls, [[1, 90, 1]]);
  await assert.rejects(readCaptureFrame({ request: async () => [] }, 0, 101, 0));
});
