import { unzipSync } from 'fflate';
import { isRecord } from './client';

export type UpdateArchitecture = 'x86' | 'x64';
export type SignatureStatus = 'verified' | 'unsigned' | 'untrusted' | 'invalid' | 'unsupported';

export interface UpdateStatus {
  version: string;
  architecture: UpdateArchitecture;
  xp: boolean;
  executable: string;
  executable_crc32: number;
  max_file_bytes: number;
  max_total_bytes: number;
  chunk_bytes: number;
}

export interface UpdateEntry {
  name: string;
  size: number;
  crc32: number;
}

export interface UpdateCandidate extends UpdateEntry {
  source: string;
  data: Uint8Array;
}

export interface UpdateSignature {
  status: SignatureStatus;
  signer: string;
  detail: string;
}

export interface UpdateResultFile {
  name: string;
  size: number;
  signature: UpdateSignature;
}

export interface UpdateResult {
  files: UpdateResultFile[];
}

export class UpdateInputError extends Error {
  constructor(readonly reason: 'invalidfile' | 'unsupported', message: string = reason) {
    super(message);
    this.name = 'UpdateInputError';
  }
}

const MAX_ARCHIVE_ENTRIES = 4096;
const HARD_MAX_FILE_BYTES = 64 * 1024 * 1024;
const HARD_MAX_TOTAL_BYTES = 128 * 1024 * 1024;
const HARD_MAX_CHUNK_BYTES = 32768;
const SIGNATURE_STATUSES: SignatureStatus[] = ['verified', 'unsigned', 'untrusted', 'invalid', 'unsupported'];
const CRC_TABLE = Array.from({ length: 256 }, (_, index) => {
  let value = index;
  for (let bit = 0; bit < 8; bit++) value = value & 1 ? (value >>> 1) ^ 0xedb88320 : value >>> 1;
  return value >>> 0;
});

function crc32(data: Uint8Array): number {
  let value = 0xffffffff;
  for (const byte of data) value = CRC_TABLE[(value ^ byte) & 0xff]! ^ (value >>> 8);
  return (value ^ 0xffffffff) >>> 0;
}

function invalidResponse(): Error { return new Error('Invalid update API response.'); }

function finiteInteger(value: unknown, min = 0, max = Number.MAX_SAFE_INTEGER): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= min && value <= max;
}

function string(value: unknown): value is string { return typeof value === 'string'; }

export function parseUpdateStatus(data: unknown[]): UpdateStatus {
  const value = data[0];
  if (!isRecord(value) || !string(value.version) || !string(value.executable) ||
      !['x86', 'x64'].includes(String(value.architecture)) || typeof value.xp !== 'boolean' ||
      !finiteInteger(value.executable_crc32, 0, 0xffffffff) ||
      !finiteInteger(value.max_file_bytes, 1, HARD_MAX_FILE_BYTES) ||
      !finiteInteger(value.max_total_bytes, 1, HARD_MAX_TOTAL_BYTES) ||
      !finiteInteger(value.chunk_bytes, 1, HARD_MAX_CHUNK_BYTES) ||
      value.max_total_bytes < value.max_file_bytes) throw invalidResponse();
  return {
    version: value.version,
    architecture: value.architecture as UpdateArchitecture,
    xp: value.xp,
    executable: value.executable,
    executable_crc32: value.executable_crc32,
    max_file_bytes: value.max_file_bytes,
    max_total_bytes: value.max_total_bytes,
    chunk_bytes: value.chunk_bytes,
  };
}

export function parseUpdateBegin(data: unknown[]): string {
  const value = data[0];
  if (!isRecord(value) || !string(value.id) || !value.id || value.id.length > 128 || /[\u0000-\u001f\u007f]/.test(value.id)) throw invalidResponse();
  return value.id;
}

export function parseUpdateChunk(data: unknown[]): number {
  const value = data[0];
  if (!isRecord(value) || !finiteInteger(value.received)) throw invalidResponse();
  return value.received;
}

function parseSignature(value: unknown): UpdateSignature {
  if (!isRecord(value) || !string(value.status) || !SIGNATURE_STATUSES.includes(value.status as SignatureStatus) ||
      (value.signer !== undefined && !string(value.signer)) || (value.detail !== undefined && !string(value.detail))) {
    throw invalidResponse();
  }
  return { status: value.status as SignatureStatus, signer: value.signer ?? '', detail: value.detail ?? '' };
}

export function parseUpdateFinish(data: unknown[]): UpdateResult {
  const value = data[0];
  if (!isRecord(value) || !Array.isArray(value.files)) throw invalidResponse();
  const files = value.files.map(file => {
    if (!isRecord(file) || !string(file.name) || !finiteInteger(file.size) || file.size > HARD_MAX_FILE_BYTES) throw invalidResponse();
    return { name: file.name, size: file.size, signature: parseSignature(file.signature) };
  });
  return { files };
}

export function parseUpdateApply(data: unknown[]): boolean {
  const value = data[0];
  if (!isRecord(value) || value.restarting !== true) throw invalidResponse();
  return value.restarting;
}

function expectedFiles(status: UpdateStatus): Map<string, string> {
  const executable = status.architecture === 'x64' ? 'spice64.exe' : 'spice.exe';
  if (status.executable.toLowerCase() !== executable) throw new UpdateInputError('unsupported', 'The cabinet reported an unsupported executable.');
  const prefix = status.xp ? 'extras/winxp/' : '';
  // Keep the destination names rooted at the cabinet folder. XP archives must
  // provide the XP paths; accepting root files would silently downgrade them.
  return new Map([[`${prefix}${executable}`, executable], [`${prefix}spicecfg.exe`, 'spicecfg.exe']]);
}

function candidate(name: string, source: string, data: Uint8Array, status: UpdateStatus, total: number): UpdateCandidate {
  if (!data.byteLength || data.byteLength > status.max_file_bytes || data.byteLength > HARD_MAX_FILE_BYTES || total + data.byteLength > status.max_total_bytes) {
    throw new UpdateInputError('invalidfile', 'The selected executable exceeds the update size limit.');
  }
  return { name, source, size: data.byteLength, crc32: crc32(data), data: new Uint8Array(data) };
}

function isZip(data: Uint8Array): boolean {
  return data.length >= 4 && data[0] === 0x50 && data[1] === 0x4b &&
    ((data[2] === 0x03 && data[3] === 0x04) || (data[2] === 0x05 && data[3] === 0x06) || (data[2] === 0x07 && data[3] === 0x08));
}

export function selectUpdateFiles(name: string, data: Uint8Array, status: UpdateStatus, type = ''): UpdateCandidate[] {
  if (!data.byteLength || data.byteLength > status.max_total_bytes || data.byteLength > HARD_MAX_TOTAL_BYTES) throw new UpdateInputError('invalidfile', 'The selected file exceeds the update size limit.');
  const lower = name.toLowerCase();
  if (lower.endsWith('.sig')) throw new UpdateInputError('unsupported', 'Signature sidecar files are not accepted.');
  const expected = expectedFiles(status);
  const isArchive = lower.endsWith('.zip') || type === 'application/zip' || isZip(data);
  if (!isArchive) {
    if (!lower.endsWith('.exe')) throw new UpdateInputError('unsupported', 'Select a release ZIP or an executable.');
    const target = status.architecture === 'x64' ? 'spice64.exe' : 'spice.exe';
    if (lower !== target) throw new UpdateInputError('unsupported', 'Raw uploads must be the active Spice executable.');
    return [candidate(target, name, data, status, 0)];
  }

  const selected = new Map<string, UpdateCandidate>();
  const seenTargets = new Set<string>();
  let entries = 0;
  let total = 0;
  try {
    const files = unzipSync(data, { filter: info => {
      if (++entries > MAX_ARCHIVE_ENTRIES) throw new UpdateInputError('invalidfile', 'The archive contains too many files.');
      const target = [...expected].find(([source]) => source.toLowerCase() === info.name.toLowerCase())?.[1];
      if (!target) return false;
      if (seenTargets.has(target) || !finiteInteger(info.originalSize) || info.originalSize > status.max_file_bytes || info.originalSize > HARD_MAX_FILE_BYTES || total + info.originalSize > status.max_total_bytes) {
        throw new UpdateInputError('invalidfile', 'The archive contains a duplicate or oversized executable.');
      }
      seenTargets.add(target);
      total += info.originalSize;
      return true;
    } });
    for (const [source, bytes] of Object.entries(files)) {
      const target = [...expected].find(([path]) => path === source || path.toLowerCase() === source.toLowerCase())?.[1];
      if (!target || selected.has(target)) throw new UpdateInputError('invalidfile', 'The archive contains an invalid executable entry.');
      selected.set(target, candidate(target, source, bytes, status, total - bytes.byteLength));
    }
  } catch (error) {
    if (error instanceof UpdateInputError) throw error;
    throw new UpdateInputError('invalidfile', 'The selected ZIP could not be read.');
  }
  const active = status.architecture === 'x64' ? 'spice64.exe' : 'spice.exe';
  if (!selected.has(active)) throw new UpdateInputError('invalidfile', 'The ZIP is missing the active Spice executable.');
  return [...expected.values()].map(target => selected.get(target)).filter((file): file is UpdateCandidate => !!file);
}

export function matchesUpdateResult(result: UpdateResult, entries: UpdateEntry[]): boolean {
  if (result.files.length !== entries.length) return false;
  const expected = new Map(entries.map(file => [file.name, file.size]));
  if (expected.size !== entries.length) return false;
  for (const file of result.files) {
    if (!expected.has(file.name) || expected.get(file.name) !== file.size) return false;
    expected.delete(file.name);
  }
  return expected.size === 0;
}

export async function readUpdateFile(file: Pick<File, 'name' | 'type' | 'size' | 'arrayBuffer'>, status: UpdateStatus): Promise<UpdateCandidate[]> {
  if (!finiteInteger(file.size) || file.size <= 0 || file.size > status.max_total_bytes) throw new UpdateInputError('invalidfile', 'The selected file exceeds the update size limit.');
  return selectUpdateFiles(file.name, new Uint8Array(await file.arrayBuffer()), status, file.type);
}

export function encodeBase64(data: Uint8Array): string {
  let binary = '';
  for (let offset = 0; offset < data.length; offset += 0x8000) binary += String.fromCharCode(...data.subarray(offset, offset + 0x8000));
  return btoa(binary);
}
