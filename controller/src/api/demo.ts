import { t, LocalizedError } from '../i18n';
import type { Rpc } from './client';
import { defaultScene } from './resize';
import type { ResizeState, ResizePatch } from './resize';

/** Explicitly selected demo: no network access, no claim of a real machine connection. */
export class DemoApi implements Rpc {
  private resize: ResizeState = { enabled: true, scene: 1, linear_filter: true, scenes: Array.from({ length: 4 }, defaultScene) };
  private coinQueue = 0;
  private buttons = new Map([['Service', false], ['Test', false], ['Coin Mech', false]]);
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
    if (module === 'coin') {
      if (fn === 'insert') this.coinQueue += Number(params[0] ?? 1);
      return fn === 'get' ? [this.coinQueue] : [];
    }
    if (module === 'info' && fn === 'log') {
      const text = 'DEMO: offline log viewer\nDEMO: no cabinet connection\n';
      const offset = Number(params[0] ?? 0), next = Math.min(text.length, offset + Number(params[1] ?? 65536));
      return [{ offset, next, size: text.length, data: btoa(text.slice(offset, next)) }];
    }
    if (module === 'info') return [fn === 'avs' ? { model: 'DEMO', dest: 'J', spec: 'A' } : { version: t('status.demo') }];
    if (module === 'card' && fn === 'get_cards') return [];
    if (module === 'capture') throw new LocalizedError('error.demoCapture');
    return [];
  }
}
