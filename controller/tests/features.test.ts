import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseCardManager, parseGames, parseIoAnalogs, parseIoButtons, parseIoLights, parseOptions, parsePatches, generateCardId } from '../src/api/features';
import { clearOptionDraft, mergeOptionDraft, optionDraftChanges, setOptionDraftValue } from '../src/api/options';
import type { OptionDraftState } from '../src/api/options';
import { cloneCardManagerCards, mergeCardManagerDraft } from '../src/api/cardManagerDraft';
import { reactive } from 'vue';
import type { OptionsState } from '../src/api/features';
import type { Rpc } from '../src/api/client';
import { useController } from '../src/useController';

const delay = (milliseconds: number): Promise<void> => new Promise(resolve => setTimeout(resolve, milliseconds));

function resizeResponse(): unknown[] {
  const scene = { offset_x: 0, offset_y: 0, scale_x: 1, scale_y: 1, keep_aspect_ratio: false, duplicate: 0 };
  return [{ enabled: true, scene: 1, linear_filter: true, scenes: [scene, scene, scene, scene] }];
}

class FeatureRpc implements Rpc {
  readonly requests: Array<{ module: string; fn: string; params: unknown[] }> = [];
  readonly optionDelay = new Map<string, number>();
  readonly failOptions = new Set<string>();
  readonly savedOptions: unknown[][] = [];
  readonly pulses: unknown[][] = [];
  managerRevision = 1;
  managerSaveRevision?: number;
  failManagerActive = false;
  ioReadDelay = 0;
  private activeIoReads = 0;
  maxIoReads = 0;

  async request(module: string, fn: string, params: unknown[] = []): Promise<unknown[]> {
    this.requests.push({ module, fn, params });
    if (module === 'buttons' && fn === 'read') {
      this.activeIoReads++; this.maxIoReads = Math.max(this.maxIoReads, this.activeIoReads);
      await delay(this.ioReadDelay); this.activeIoReads--;
      return [['Service', 0, false], ['Polled', 0, false]];
    }
    if (module === 'buttons' && fn === 'pulse') { this.pulses.push(params); return []; }
    if (module === 'analogs' && fn === 'read') { await delay(this.ioReadDelay); return [['Knob', 0.5, false]]; }
    if (module === 'lights' && fn === 'read') { await delay(this.ioReadDelay); return [['Lamp', 0.25, false]]; }
    if ((module === 'analogs' || module === 'lights') && fn === 'pulse') { this.pulses.push(params); return []; }
    if (module === 'analogs' || module === 'lights') return [];
    if (module === 'info' && fn === 'avs') return [{ model: 'TEST', dest: 'J', spec: 'A' }];
    if (module === 'info' && fn === 'launcher') return [{ version: 'test' }];
    if (module === 'info' && fn === 'memory') return [{ mem_total: 8, mem_total_used: 2, mem_used: 1, vmem_total: 16, vmem_total_used: 4, vmem_used: 2 }];
    if (module === 'resize' && fn === 'image_resize_get') return resizeResponse();
    if (module === 'coin' && fn === 'get') return [0];
    if (module === 'config' && fn === 'get_games') return [{ current: 'A', games: ['A', 'B'] }];
    if (module === 'config' && fn === 'set_options') {
      this.savedOptions.push(params);
      const game = String(params[0]);
      return [{ game, revision: Number(params[1]) + 1, restart_required: true, options: [{ name: 'mode', title: 'Mode', description: '', category: 'Game', type: 'text', value: String((params[2] as Array<{ value?: unknown }>)[0]?.value ?? game), sensitive: false, has_value: true, hidden: false, disabled: false, elements: [] }] }];
    }
    if (module === 'config' && fn === 'get_options') {
      const game = String(params[0]);
      await delay(this.optionDelay.get(game) ?? 0);
      if (this.failOptions.has(game)) throw new Error(`stale ${game}`);
      return [{ game, revision: 1, restart_required: true, options: [{ name: 'mode', title: 'Mode', description: '', category: 'Game', type: 'text', value: game, sensitive: false, has_value: true, hidden: false, disabled: false, elements: [] }] }];
    }
    if (module === 'patches' && fn === 'get') return [{ game: 'A', auto_apply: false, dirty: false, standalone: false, patches: [] }];
    if (module === 'card' && fn === 'manager_get') return [{ revision: this.managerRevision, cards: [{ name: 'A', id: 'E004000000000001', color: [0.1, 0.2, 0.3] }], active: ['', ''] }];
    if (module === 'card' && fn === 'manager_set_active') {
      if (this.failManagerActive) throw new Error('active failed');
      return [{ revision: ++this.managerRevision, cards: [{ name: 'A', id: 'E004000000000001', color: [0.1, 0.2, 0.3] }], active: [String(params[1] ?? ''), ''] }];
    }
    if (module === 'card' && fn === 'manager_save') {
      this.managerSaveRevision = Number(params[0]);
      if (this.managerSaveRevision !== this.managerRevision) throw new Error('revision conflict');
      return [{ revision: 3, cards: params[1], active: ['', ''] }];
    }
    return [];
  }
}

test('feature parsers validate native option and patch contracts', () => {
  assert.deepEqual(parseGames([{ current: 'IIDX', games: ['IIDX', 'SDVX'] }]), { current: 'IIDX', games: ['IIDX', 'SDVX'] });
  const options = parseOptions([{
    game: 'IIDX', revision: 4, restart_required: true,
    options: [{ name: 'mode', title: 'Mode', description: 'Select a mode', category: 'Game', type: 'enum', value: 'a',
      sensitive: false, has_value: true, hidden: false, disabled: false, elements: [{ value: 'a', label: 'A' }] }],
  }]);
  assert.equal(options.options[0]!.elements[0]!.value, 'a');
  const patches = parsePatches([{ game: 'IIDX', auto_apply: true, dirty: true, standalone: false, patches: [{
    id: 'p', name: 'Patch', description: '', caution: '', group: 'Main', type: 'integer', enabled: true,
    status: 'enabled', error: '', unverified: false, choices: [], selected: '', min: 0, max: 10, value: 3,
  }] }]);
  assert.equal(patches.patches[0]!.value, 3);
  const failedInteger = parsePatches([{ game: 'IIDX', auto_apply: false, dirty: false, standalone: false, patches: [
    { id: 'bad-int', name: '', description: '', caution: '', group: '', type: 'integer', enabled: false, status: 'error', error: 'missing', unverified: false, choices: [], selected: '', min: 10, max: 20, value: 0 },
    { id: 'usable', name: '', description: '', caution: '', group: '', type: 'memory', enabled: false, status: 'disabled', error: '', unverified: false, choices: [], selected: '' },
  ] }]);
  assert.equal(failedInteger.patches.length, 2);
  const unverifiedInteger = parsePatches([{ game: 'IIDX', auto_apply: false, dirty: false, standalone: false, patches: [
    { id: 'unknown-int', name: '', description: '', caution: '', group: '', type: 'integer', enabled: false, status: 'disabled', error: '', unverified: true, choices: [], selected: '', min: 10, max: 20, value: 0 },
  ] }]);
  assert.equal(unverifiedInteger.patches[0]!.value, 0);
  const omittedInteger = parsePatches([{ game: 'IIDX', auto_apply: false, dirty: false, standalone: false, patches: [
    { id: 'omitted-int', name: '', description: '', caution: '', group: '', type: 'integer', enabled: false, status: 'error', error: 'missing', unverified: true, choices: [], selected: '', min: 10, max: 20 },
  ] }]);
  assert.equal(omittedInteger.patches[0]!.value, undefined);
  assert.equal(omittedInteger.patches[0]!.min, 10);
  assert.throws(() => parseOptions([{ game: 'IIDX', revision: 1, restart_required: false, options: [{ type: 'bad' }] }]));
  assert.throws(() => parseOptions([{ game: 'IIDX', revision: -1, restart_required: false, options: [] }]));
  assert.throws(() => parsePatches([{ game: 'IIDX', auto_apply: false, dirty: false, standalone: false, patches: [{ id: 'p', name: '', description: '', caution: '', group: '', type: 'integer', enabled: false, status: 'disabled', error: '', unverified: false, choices: [], selected: '', min: 4, max: 2, value: 3 }] }]));
  assert.throws(() => parsePatches([{ game: 'IIDX', auto_apply: false, dirty: false, standalone: false, patches: [{ id: 'p', name: '', description: '', caution: '', group: '', type: 'integer', enabled: false, status: 'disabled', error: '', unverified: false, choices: [], selected: '', min: 0, max: 2147483648, value: 1 }] }]));
});

test('card manager and I/O parsers preserve masked and numeric values', () => {
  const cards = parseCardManager([{ revision: 2, cards: [{ name: 'A', id: 'e004000000000001', color: [0.1, 0.2, 0.3] }], active: ['', 'e004000000000001'] }]);
  assert.equal(cards.cards[0]!.id, 'E004000000000001');
  assert.deepEqual(parseIoButtons([['Button', 1, false]])[0], { name: 'Button', value: 1, overridden: false });
  assert.deepEqual(parseIoAnalogs([['Knob', 0.5, true]]), [{ name: 'Knob', value: 0.5, overridden: true }]);
  assert.deepEqual(parseIoLights([['Lamp', 0, false]]), [{ name: 'Lamp', value: 0, overridden: false }]);
  assert.throws(() => parseCardManager([{ revision: 1, cards: [], active: ['bad'] }]));
  assert.throws(() => parseCardManager([{ revision: 1.5, cards: [], active: ['', ''] }]));
  assert.throws(() => parseCardManager([{ revision: -1, cards: [], active: ['', ''] }]));
  assert.throws(() => parseIoAnalogs([['Knob', 1.1, false]]));
  assert.match(generateCardId(), /^[0-9A-F]{16}$/);
});

test('staged option draft keeps its values and base revision across refreshes', () => {
  const draft: OptionDraftState = { game: '', revision: undefined, values: {}, touched: {} };
  const snapshot = (game: string, revision: number, value: string): OptionsState => ({
    game, revision, restart_required: true,
    options: [{ name: 'mode', title: 'Mode', description: '', category: 'Game', type: 'text', value, sensitive: false, has_value: true, hidden: false, disabled: false, elements: [] }],
  });
  mergeOptionDraft(draft, snapshot('A', 7, 'server'));
  setOptionDraftValue(draft, 'mode', 'staged');
  mergeOptionDraft(draft, snapshot('A', 8, 'new-server'));
  assert.equal(draft.values.mode, 'staged');
  assert.equal(draft.revision, 7);
  assert.deepEqual(optionDraftChanges(draft, snapshot('A', 8, 'new-server').options), [{ name: 'mode', value: 'staged' }]);
});

test('delayed game switch cannot save an old draft against the new game', async () => {
  const api = new FeatureRpc(); api.optionDelay.set('B', 35);
  const controller = useController(() => api);
  const draft: OptionDraftState = { game: '', revision: undefined, values: {}, touched: {} };
  try {
    await controller.startDemo(); await controller.refreshGames(); await controller.refreshOptions('A');
    mergeOptionDraft(draft, controller.options.value);
    setOptionDraftValue(draft, 'mode', 'typed while A is visible');
    const switching = controller.refreshOptions('B');
    await delay(2);
    assert.equal(controller.options.value?.game, 'A');
    await switching;
    const server = controller.options.value!;
    assert.equal(server.game, 'B');
    assert.deepEqual(optionDraftChanges(draft, server.options, server.game), []);
    assert.equal(api.savedOptions.length, 0);

    clearOptionDraft(draft); mergeOptionDraft(draft, server); setOptionDraftValue(draft, 'mode', 'typed for B');
    const changes = optionDraftChanges(draft, server.options, server.game);
    assert.deepEqual(changes, [{ name: 'mode', value: 'typed for B' }]);
    assert.equal(await controller.saveOptions(changes, draft.revision), true);
    assert.equal(api.savedOptions[0]![0], 'B');
  } finally { controller.disconnect(); }
});

test('card manager draft clears on disconnect and clones reactive host cards', () => {
  const cards = reactive([{ name: 'Host', id: 'E004000000000001', color: [0.1, 0.2, 0.3] as [number, number, number] }]);
  const loaded = mergeCardManagerDraft({ cards: [], active: ['', ''], revision: undefined, dirty: false }, { revision: 4, cards, active: ['', ''] });
  assert.equal(loaded.cards[0]!.name, 'Host');
  const dirty = mergeCardManagerDraft({ ...loaded, dirty: true, cards: [{ ...loaded.cards[0]!, name: 'Staged' }] }, { revision: 5, cards, active: ['E004000000000001', ''] });
  assert.equal(dirty.cards[0]!.name, 'Staged');
  assert.equal(dirty.revision, 4);
  assert.equal(dirty.active[0], 'E004000000000001');
  const cleared = mergeCardManagerDraft(dirty);
  assert.equal(cleared.dirty, false);
  assert.equal(cleared.revision, undefined);
});

test('demo feature actions use the same RPC path and stop cleanly on disconnect', async () => {
  const controller = useController();
  try {
    await controller.startDemo();
    await controller.readStatus();
    assert.equal(controller.memory.value?.mem_used, 512 * 1024 ** 2);
    await controller.refreshGames();
    await controller.refreshOptions('DEMO');
    assert.equal(controller.options.value?.game, 'DEMO');
    await controller.saveOptions([{ name: 'demo-enabled', value: '/ENABLED' }]);
    await controller.refreshPatches();
    await controller.setPatch('demo-patch', { enabled: true });
    await controller.refreshCardManager();
    assert.equal(controller.cardManager.value?.cards[0]?.id, 'E004000000000001');
    const draft = reactive(controller.cardManager.value!.cards.map(card => ({
      name: card.name,
      id: card.id,
      color: [card.color[0], card.color[1], card.color[2]] as [number, number, number],
    })));
    draft[0]!.name = 'Saved demo card'; draft[0]!.color[0] = 0.77;
    assert.equal(await controller.saveCardManager(cloneCardManagerCards(draft), controller.cardManager.value!.revision), true);
    assert.equal(controller.cardManager.value?.cards[0]?.name, 'Saved demo card');
    assert.equal(controller.cardManager.value?.cards[0]?.color[0], 0.77);
    await controller.readIo();
    assert.equal(controller.ioButtons.value[0]?.name, 'Service');
    await controller.testIo('analog', 'Demo knob', 0.75);
    await controller.testIo('light', 'Demo lamp', 0.5);
    await controller.readIo();
    assert.equal(controller.ioAnalogs.value[0]?.overridden, false);
    assert.equal(controller.ioLights.value[0]?.overridden, false);
    controller.startIoPolling(200); controller.stopIoPolling();
    controller.disconnect();
    await controller.refreshOptions('DEMO');
    assert.equal(controller.options.value, undefined);
  } finally { controller.disconnect(); }
});

test('feature reads keep newest game response and ignore stale errors after disconnect', async () => {
  const api = new FeatureRpc(); api.optionDelay.set('A', 35); api.optionDelay.set('B', 5); api.failOptions.add('A');
  const controller = useController(() => api);
  try {
    await controller.startDemo(); await controller.refreshGames();
    await Promise.all([controller.refreshOptions('A'), controller.refreshOptions('B')]);
    assert.equal(controller.options.value?.game, 'B');
    assert.equal(controller.featureErrors.value.options, '');
    await controller.testIo('button', 'Service');
    await controller.testIo('analog', 'Knob', 0.7);
    await controller.testIo('light', 'Lamp', 0.3);
    assert.deepEqual(api.pulses.slice(-3), [['Service', 180], ['Knob', 0.7, 180], ['Lamp', 0.3, 180]]);
    api.optionDelay.set('A', 25);
    const stale = controller.refreshOptions('A'); controller.disconnect(); await stale;
    assert.deepEqual(controller.featureErrors.value, {});
  } finally { controller.disconnect(); }
});

test('card manager saves use the immutable list revision across active refreshes', async () => {
  const api = new FeatureRpc();
  const controller = useController(() => api);
  try {
    await controller.startDemo(); await controller.refreshCardManager();
    await controller.setManagerActive(0, 'E004000000000001');
    const saved = await controller.saveCardManager([{ name: 'Edited', id: 'E004000000000001', color: [0.1, 0.2, 0.3] }], 1);
    assert.equal(saved, false);
    assert.equal(api.managerSaveRevision, 1);
  } finally { controller.disconnect(); }
});

test('failed active-card override leaves the confirmed selection unchanged', async () => {
  const api = new FeatureRpc();
  const controller = useController(() => api);
  try {
    await controller.startDemo(); await controller.refreshCardManager();
    api.failManagerActive = true;
    assert.equal(await controller.setManagerActive(0, 'E004000000000001'), false);
    assert.deepEqual(controller.cardManager.value?.active, ['', '']);
  } finally { controller.disconnect(); }
});

test('I/O polling tracks the initial read and invalidates it when stopped', async () => {
  const api = new FeatureRpc(); api.ioReadDelay = 250;
  const controller = useController(() => api);
  try {
    await controller.startDemo(); api.ioReadDelay = 40;
    controller.startIoPolling(200); controller.stopIoPolling();
    await delay(60);
    assert.equal(controller.ioButtons.value.length, 0);
    api.ioReadDelay = 250; controller.startIoPolling(200); await delay(450); controller.stopIoPolling();
    assert.equal(api.maxIoReads, 1);
  } finally { controller.disconnect(); }
});
