import { encodeBase64, matchesUpdateResult } from './update';
import type { UpdateCandidate, UpdateResult } from './update';

export class UpdateUploadCancelled extends Error {
  constructor() { super('Update upload cancelled.'); this.name = 'UpdateUploadCancelled'; }
}

export class UpdateUploadCleanupError extends Error {
  constructor(readonly id: string, readonly original: unknown) {
    super('Update upload cleanup failed.'); this.name = 'UpdateUploadCleanupError';
  }
}

export interface UpdateUploadTransport {
  begin(entries: UpdateCandidate[]): Promise<string | undefined>;
  chunk(id: string, name: string, offset: number, base64: string): Promise<number | undefined>;
  finish(id: string): Promise<UpdateResult | undefined>;
  cancel(id: string): Promise<boolean>;
}

export interface UpdateUploadHooks {
  cancelled?: () => boolean;
  started?: (id: string) => void;
  finishing?: () => void;
  progress?: (sent: number, file: string) => void;
}

export async function uploadUpdate(files: UpdateCandidate[], chunkBytes: number, transport: UpdateUploadTransport, hooks: UpdateUploadHooks = {}): Promise<UpdateResult | undefined> {
  let id: string | undefined;
  try {
    id = await transport.begin(files);
    if (!id) return undefined;
    hooks.started?.(id);
    const size = Math.max(1, Math.min(32768, Math.floor(chunkBytes)));
    let sent = 0;
    for (const file of files) {
      for (let offset = 0; offset < file.data.length; offset += size) {
        if (hooks.cancelled?.()) throw new UpdateUploadCancelled();
        const next = Math.min(file.data.length, offset + size);
        const received = await transport.chunk(id, file.name, offset, encodeBase64(file.data.subarray(offset, next)));
        if (hooks.cancelled?.()) throw new UpdateUploadCancelled();
        if (received !== next) throw new Error('Update upload failed.');
        sent += next - offset; hooks.progress?.(sent, file.name);
      }
    }
    if (hooks.cancelled?.()) throw new UpdateUploadCancelled();
    hooks.finishing?.();
    const result = await transport.finish(id);
    if (hooks.cancelled?.()) throw new UpdateUploadCancelled();
    if (!result || !matchesUpdateResult(result, files)) throw new Error('Update upload failed.');
    return result;
  } catch (error) {
    if (id) {
      let cancelled = false;
      try { cancelled = await transport.cancel(id); } catch { /* Surface cleanup failure with the session ID intact. */ }
      if (!cancelled) throw new UpdateUploadCleanupError(id, error);
    }
    throw error;
  }
}

export interface UpdateViewState {
  active: boolean;
  connected: boolean;
  password: boolean;
  demo: boolean;
}

export function shouldReloadUpdateStatus(current: UpdateViewState, previous: UpdateViewState | undefined, hasStatus: boolean, phase: string): boolean {
  if (!current.active || !current.connected || !current.password || current.demo || phase === 'applying' || phase === 'reconnecting') return false;
  return !hasStatus || !previous?.active || !previous.connected || !previous.password || previous.demo;
}
