export const DEFAULT_IO_PULSE_MS = 180;
export const MIN_IO_PULSE_MS = 1;
export const MAX_IO_PULSE_MS = 2000;

/** Keep browser-originated pulse values inside the native API contract. */
export function validateIoPulse(value: number, duration: number): boolean {
  return Number.isFinite(value) && value >= 0 && value <= 1
    && Number.isInteger(duration) && duration >= MIN_IO_PULSE_MS && duration <= MAX_IO_PULSE_MS;
}
