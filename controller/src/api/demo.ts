import { t, LocalizedError } from '../i18n';
import type { Rpc } from './client';
import { defaultScene } from './resize';
import type { ResizeState, ResizePatch } from './resize';

/** Explicitly selected demo: no network access, no claim of a real machine connection. */
export class DemoApi implements Rpc {
  private resize: ResizeState = { enabled: true, scene: 1, linear_filter: true, scenes: Array.from({ length: 4 }, defaultScene) };
  private coinQueue = 0;
  private buttons = new Map([['Service', false], ['Test', false], ['Coin Mech', false]]);
  private analogs = new Map([['Demo knob', 0.5]]);
  private lights = new Map([['Demo lamp', 0.25]]);
  private ioOverrides = new Set<string>();
  private optionValues = new Map([['demo-enabled', '']]);
  private optionRevision = 1;
  private patchEnabled = new Map<string, boolean>();
  private patchAutoApply = false;
  private managerCards = [{ name: 'Demo card', id: 'E004000000000001', color: [0.08, 0.47, 0.39] as [number, number, number] }];
  private managerActive: [string, string] = ['', ''];
  private managerRevision = 1;
  async request(module: string, fn: string, params: unknown[] = []): Promise<unknown[]> {
    await new Promise(resolve => setTimeout(resolve, 35));
    if (module === 'resize') {
      if (fn === 'image_resize_save') return [];
      if (fn === 'image_resize_set') {
        const p = params[0] as ResizePatch;
        if (p.scene !== undefined) this.resize.scene = p.scene;
        if (p.enabled !== undefined) this.resize.enabled = p.enabled;
        if (p.linear_filter !== undefined) this.resize.linear_filter = p.linear_filter;
        const { scene, enabled, linear_filter, ...settings } = p;
        Object.assign(this.resize.scenes[this.resize.scene - 1]!, settings);
        const s = this.resize.scenes[this.resize.scene - 1]!;
        if (s.keep_aspect_ratio) {
          if (p.scale_y !== undefined && p.scale_x === undefined) s.scale_x = s.scale_y;
          else s.scale_y = s.scale_x;
        }
      }
      return [structuredClone(this.resize)];
    }
    if (module === 'buttons') {
      if (fn === 'read') return [...this.buttons].map(([name, enabled]) => [name, enabled ? 1 : 0, enabled]);
      if (fn === 'write') for (const [name, pressed] of params as [string, boolean][]) this.buttons.set(name, pressed);
      if (fn === 'write_reset') for (const name of this.buttons.keys()) this.buttons.set(name, false);
      if (fn === 'pulse') {
        const name = String(params[0]); this.buttons.set(name, true);
        try { await new Promise(resolve => setTimeout(resolve, Number(params[1] ?? 180))); }
        finally { this.buttons.set(name, false); }
      }
      return [];
    }
    if (module === 'analogs') {
      if (fn === 'read') return [...this.analogs].map(([name, value]) => [name, value, this.ioOverrides.has(`analog:${name}`)]);
      if (fn === 'write_reset') { for (const key of [...this.ioOverrides].filter(key => key.startsWith('analog:'))) this.ioOverrides.delete(key); return []; }
      if (fn === 'pulse') {
        const name = String(params[0]); const original = this.analogs.get(name);
        if (original === undefined) return [];
        this.analogs.set(name, Math.max(0, Math.min(1, Number(params[1] ?? 1)))); this.ioOverrides.add(`analog:${name}`);
        try { await new Promise(resolve => setTimeout(resolve, Number(params[2] ?? 180))); }
        finally { this.analogs.set(name, original); this.ioOverrides.delete(`analog:${name}`); }
        return [];
      }
    }
    if (module === 'lights') {
      if (fn === 'read') return [...this.lights].map(([name, value]) => [name, value, this.ioOverrides.has(`light:${name}`)]);
      if (fn === 'write_reset') { for (const key of [...this.ioOverrides].filter(key => key.startsWith('light:'))) this.ioOverrides.delete(key); return []; }
      if (fn === 'pulse') {
        const name = String(params[0]); const original = this.lights.get(name);
        if (original === undefined) return [];
        this.lights.set(name, Math.max(0, Math.min(1, Number(params[1] ?? 1)))); this.ioOverrides.add(`light:${name}`);
        try { await new Promise(resolve => setTimeout(resolve, Number(params[2] ?? 180))); }
        finally { this.lights.set(name, original); this.ioOverrides.delete(`light:${name}`); }
        return [];
      }
    }
    if (module === 'config') {
      if (fn === 'get_games') return [{ current: 'DEMO', games: ['DEMO'] }];
      if (fn === 'get_options') return [{ game: String(params[0] ?? 'DEMO'), revision: this.optionRevision, restart_required: true,
        options: [{ name: 'demo-enabled', title: 'Demo option', description: 'A safe demo option.', category: 'Demo', type: 'bool',
          value: this.optionValues.get('demo-enabled') ?? '', sensitive: false, has_value: !!this.optionValues.get('demo-enabled'),
          hidden: false, disabled: false, elements: [] }] }];
      if (fn === 'set_options') {
        const changes = params[2];
        if (Array.isArray(changes)) for (const change of changes) {
          if (Array.isArray(change) && typeof change[0] === 'string' && typeof change[1] === 'string') this.optionValues.set(change[0], change[1]);
          else if (change && typeof change === 'object' && 'name' in change && 'value' in change && typeof change.name === 'string' && typeof change.value === 'string') this.optionValues.set(change.name, change.value);
        }
        this.optionRevision++;
        return this.request('config', 'get_options', [params[0] ?? 'DEMO']);
      }
    }
    if (module === 'patches') {
      const patch = { id: 'demo-patch', name: 'Demo patch', description: 'A safe demo patch.', caution: '', group: 'Demo', type: 'memory', enabled: this.patchEnabled.get('demo-patch') ?? false,
        status: 'disabled', error: '', unverified: false, choices: [], selected: '' };
      if (fn === 'set_auto_apply') { this.patchAutoApply = !!params[0]; return [{ game: 'DEMO', auto_apply: this.patchAutoApply, dirty: false, standalone: true, patches: [patch] }]; }
      if (fn === 'get' || fn === 'save' || fn === 'reload') return [{ game: 'DEMO', auto_apply: this.patchAutoApply, dirty: false, standalone: true, patches: [patch] }];
      if (fn === 'set') { const enabled = !!(params[1] as { enabled?: boolean } | undefined)?.enabled; this.patchEnabled.set('demo-patch', enabled); patch.enabled = enabled; return [{ game: 'DEMO', auto_apply: this.patchAutoApply, dirty: false, standalone: true, patches: [patch] }]; }
    }
    if (module === 'card' && fn === 'manager_get') return [{ revision: this.managerRevision, cards: structuredClone(this.managerCards), active: [...this.managerActive] }];
    if (module === 'card' && fn === 'manager_save') {
      const cards = params[1];
      if (Array.isArray(cards)) this.managerCards = structuredClone(cards) as typeof this.managerCards;
      this.managerRevision++;
      return [{ revision: this.managerRevision, cards: structuredClone(this.managerCards), active: [...this.managerActive] }];
    }
    if (module === 'card' && fn === 'manager_set_active') {
      const reader = Number(params[0]); const id = String(params[1] ?? '');
      if (reader === 0 || reader === 1) this.managerActive[reader] = id;
      return [{ revision: this.managerRevision, cards: structuredClone(this.managerCards), active: [...this.managerActive] }];
    }
    if (module === 'coin') {
      if (fn === 'insert') this.coinQueue += Number(params[0] ?? 1);
      return fn === 'get' ? [this.coinQueue] : [];
    }
    if (module === 'info' && fn === 'log') {
      const text = 'DEMO: offline log viewer\nDEMO: no cabinet connection\n';
      const offset = Number(params[0] ?? 0), next = Math.min(text.length, offset + Number(params[1] ?? 65536));
      return [{ offset, next, size: text.length, data: btoa(text.slice(offset, next)) }];
    }
    if (module === 'info') {
      if (fn === 'avs') return [{ model: 'DEMO', dest: 'J', spec: 'A' }];
      if (fn === 'memory') return [{ mem_total: 8 * 1024 ** 3, mem_total_used: 3 * 1024 ** 3, mem_used: 512 * 1024 ** 2, vmem_total: 16 * 1024 ** 3, vmem_total_used: 5 * 1024 ** 3, vmem_used: 1024 * 1024 ** 2 }];
      return [{ version: t('status.demo') }];
    }
    if (module === 'card' && fn === 'get_cards') return [];
    if (module === 'capture') throw new LocalizedError('error.demoCapture');
    return [];
  }
}
