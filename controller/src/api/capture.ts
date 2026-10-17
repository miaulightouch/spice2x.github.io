import { LocalizedError } from '../i18n';
import type { Rpc } from './client';
import { isRecord, normalizeEndpoint } from './client';
import { embeddedHttpOrigin, isEmbeddedBuild } from '../mode';

export interface CaptureFrame {
  timestamp: number;
  width: number;
  height: number;
  jpeg: string;
}

export interface CaptureStreams {
  port: number;
  formats: Array<{ name: string; path: string }>;
  screens: Array<{ screen: number; width: number; height: number; busy: boolean }>;
}

export function parseCaptureStreams(data: unknown[]): CaptureStreams | undefined {
  if (!data.length) return undefined;
  const raw = data[0];
  if (!isRecord(raw) || !Number.isInteger(raw.port) || Number(raw.port) < 1 || Number(raw.port) > 65535 ||
      !Array.isArray(raw.formats) || !Array.isArray(raw.screens)) throw new LocalizedError('error.unexpected');
  const formats = raw.formats.map(format => {
    if (!isRecord(format) || typeof format.name !== 'string' || typeof format.path !== 'string') throw new LocalizedError('error.unexpected');
    return { name: format.name, path: format.path };
  });
  const screens = raw.screens.map(screen => {
    if (!isRecord(screen) || ![screen.screen, screen.width, screen.height].every(Number.isSafeInteger) ||
        Number(screen.screen) < 0 || Number(screen.width) < 1 || Number(screen.height) < 1 || typeof screen.busy !== 'boolean') throw new LocalizedError('error.unexpected');
    return { screen: Number(screen.screen), width: Number(screen.width), height: Number(screen.height), busy: screen.busy };
  });
  return { port: Number(raw.port), formats, screens };
}

export function mjpegUrl(endpoint: string, streams: CaptureStreams, screen: number, quality: number, fps: number): string {
  if (!streams.formats.some(format => format.name === 'mjpeg' && format.path === '/stream.mjpg') ||
      !streams.screens.some(item => item.screen === screen && !item.busy) ||
      !Number.isInteger(quality) || quality < 1 || quality > 100 || !Number.isInteger(fps) || fps < 1 || fps > 60) {
    throw new LocalizedError('error.noCapture');
  }
  let url: URL;
  if (isEmbeddedBuild) {
    const origin = embeddedHttpOrigin();
    if (!origin) throw new LocalizedError('error.noCapture');
    url = new URL(`${origin}/stream.mjpg`);
  } else {
    url = new URL(normalizeEndpoint(endpoint));
    url.protocol = 'http:'; url.port = String(streams.port); url.pathname = '/stream.mjpg';
  }
  url.search = ''; url.hash = '';
  url.searchParams.set('screen', String(screen)); url.searchParams.set('q', String(quality)); url.searchParams.set('fps', String(fps));
  return url.href;
}

/** The native capture API returns one screen number per response item. */
export function parseCaptureScreens(data: unknown[]): number[] {
  const screens = data.filter((value): value is number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0);
  if (!screens.length) throw new LocalizedError('error.noCapture');
  return [...new Set(screens)];
}

/** Decode the native get_jpg response without trusting its dimensions or payload. */
export function parseCaptureFrame(data: unknown[]): CaptureFrame {
  const [timestamp, width, height, jpeg] = data;
  if (typeof timestamp !== 'number' || !Number.isSafeInteger(timestamp) || timestamp < 0 ||
      typeof width !== 'number' || !Number.isSafeInteger(width) || width <= 0 ||
      typeof height !== 'number' || !Number.isSafeInteger(height) || height <= 0 ||
      typeof jpeg !== 'string' || !jpeg || !/^[A-Za-z0-9+/\r\n]+={0,2}$/.test(jpeg)) {
    throw new LocalizedError('error.jpeg');
  }
  return { timestamp, width, height, jpeg };
}

export async function readCaptureScreens(rpc: Rpc): Promise<number[]> {
  return parseCaptureScreens(await rpc.request('capture', 'get_screens'));
}

export async function readCaptureFrame(rpc: Rpc, screen: number, quality = 65, divide = 2): Promise<CaptureFrame> {
  if (!Number.isSafeInteger(screen) || screen < 0 || !Number.isInteger(quality) || quality < 1 || quality > 100 ||
      !Number.isInteger(divide) || divide < 1 || divide > 8) throw new LocalizedError('error.noCapture');
  return parseCaptureFrame(await rpc.request('capture', 'get_jpg', [screen, quality, divide]));
}
