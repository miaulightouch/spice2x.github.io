import { isRecord } from './client';

export interface ResizeScene {
  offset_x: number;
  offset_y: number;
  scale_x: number;
  scale_y: number;
  keep_aspect_ratio: boolean;
  duplicate: number;
}
export interface ResizeState {
  enabled: boolean;
  scene: number;
  linear_filter: boolean;
  scenes: ResizeScene[];
}
export type ResizePatch = Partial<ResizeScene> & Partial<Pick<ResizeState, 'enabled' | 'scene' | 'linear_filter'>>;
export const defaultScene = (): ResizeScene => ({ offset_x: 0, offset_y: 0, scale_x: 1, scale_y: 1, keep_aspect_ratio: false, duplicate: 0 });

export function parseResize(data: unknown[]): ResizeState {
  const v = data[0];
  if (!isRecord(v) || typeof v.enabled !== 'boolean' || typeof v.linear_filter !== 'boolean' ||
      !Number.isInteger(v.scene) || Number(v.scene) < 1 || Number(v.scene) > 4 ||
      !Array.isArray(v.scenes) || v.scenes.length !== 4 || !v.scenes.every(isScene)) {
    throw new Error('畫面校正回應格式不符，請使用支援 Resize API 的 spice2x 版本。');
  }
  return structuredClone(v) as unknown as ResizeState;
}
function isScene(v: unknown): v is ResizeScene {
  return isRecord(v) && ['offset_x', 'offset_y'].every(k => Number.isInteger(v[k]) && Number(v[k]) >= -32768 && Number(v[k]) <= 32767) &&
    ['scale_x', 'scale_y'].every(k => typeof v[k] === 'number' && Number.isFinite(v[k]) && Number(v[k]) >= 0.59999 && Number(v[k]) <= 2.50001) &&
    typeof v.keep_aspect_ratio === 'boolean' && Number.isInteger(v.duplicate) && Number(v.duplicate) >= 0 && Number(v.duplicate) <= 2;
}

/** Coalesces UI edits without dropping changes made while the previous RPC is pending. */
export class ResizeQueue {
  private pending: ResizePatch = {};
  private timer?: ReturnType<typeof setTimeout>;
  private running?: Promise<void>;
  private cancelled = false;
  constructor(private readonly send: (patch: ResizePatch) => Promise<void>, private readonly onError: (error: unknown) => void) {}

  update(patch: ResizePatch): void {
    if (this.cancelled) return;
    this.pending = { ...this.pending, ...patch };
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => { this.timer = undefined; void this.flush().catch(() => {}); }, 90);
  }
  async flush(): Promise<void> {
    if (this.timer) clearTimeout(this.timer);
    this.timer = undefined;
    if (this.running) { await this.running; return this.flush(); }
    if (this.cancelled || !Object.keys(this.pending).length) return;
    const patch = this.pending;
    this.pending = {};
    this.running = this.send(patch);
    try { await this.running; }
    catch (error) { this.pending = {}; this.onError(error); throw error; }
    finally { this.running = undefined; }
    return this.flush();
  }
  cancel(): void {
    this.cancelled = true;
    this.pending = {};
    if (this.timer) clearTimeout(this.timer);
  }
}
