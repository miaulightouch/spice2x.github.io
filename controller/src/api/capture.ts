import { LocalizedError } from '../i18n';
import type { Rpc } from './client';

export interface CaptureFrame {
  timestamp: number;
  width: number;
  height: number;
  jpeg: string;
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

export async function readCaptureFrame(rpc: Rpc, screen: number): Promise<CaptureFrame> {
  if (!Number.isSafeInteger(screen) || screen < 0) throw new LocalizedError('error.noCapture');
  return parseCaptureFrame(await rpc.request('capture', 'get_jpg', [screen, 65, 2]));
}
