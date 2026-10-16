import type { Rpc } from './client';
import { isRecord } from './client';
import { LocalizedError } from '../i18n';

export async function readLog(rpc: Rpc, offset: number | null = null, limit = 65536) {
  const value = (await rpc.request('info', 'log', [offset, limit]))[0];
  if (!isRecord(value) || ![value.offset, value.next, value.size].every(v => Number.isSafeInteger(v) && Number(v) >= 0) ||
      typeof value.data !== 'string' || value.data.length > Math.ceil(limit / 3) * 4 ||
      !/^[A-Za-z0-9+/]*={0,2}$/.test(value.data)) throw new LocalizedError('error.logFormat');
  let bytes: Uint8Array<ArrayBuffer>;
  try { bytes = Uint8Array.from(atob(value.data), c => c.charCodeAt(0)); }
  catch { throw new LocalizedError('error.logFormat'); }
  const start = Number(value.offset), next = Number(value.next), size = Number(value.size);
  if (next < start || next > size || next - start !== bytes.length || bytes.length > limit ||
      (offset !== null && start !== offset)) throw new LocalizedError('error.logFormat');
  return { bytes, offset: start, next, size };
}

export async function downloadLog(rpc: Rpc, current: () => boolean): Promise<Blob> {
  const chunks: Uint8Array<ArrayBuffer>[] = [];
  let offset = 0, size: number | undefined;
  do {
    if (!current()) throw new LocalizedError('error.cancelled');
    const chunk = await readLog(rpc, offset, size === undefined ? 65536 : Math.min(65536, size - offset));
    if (!current()) throw new LocalizedError('error.cancelled');
    size ??= chunk.size;
    if (chunk.size < size || chunk.next > size || (chunk.next === offset && offset < size)) {
      throw new LocalizedError('error.logChanged');
    }
    chunks.push(chunk.bytes); offset = chunk.next;
  } while (offset < size);
  return new Blob(chunks, { type: 'text/plain' });
}
