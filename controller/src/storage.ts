import { LocalizedError } from './i18n';
import { isRecord } from './api/client';

export interface Profile { id: string; name: string; endpoint: string; password: string; rememberPassword: boolean }
export interface SavedCard { id: string; name: string; number: string }
const PROFILE_KEY = 'arcade-room.profiles.v1';
const CARD_KEY = 'arcade-room.cards.v1';
const defaults = (): Profile[] => [
  { id: 'jubeat-a', name: 'jubeat A', endpoint: '', password: '', rememberPassword: false },
  { id: 'jubeat-b', name: 'jubeat B', endpoint: '', password: '', rememberPassword: false },
];

export function loadProfiles(storage?: Pick<Storage, 'getItem'>): Profile[] {
  try {
    const target = storage ?? localStorage;
    const raw: unknown = JSON.parse(target.getItem(PROFILE_KEY) ?? 'null');
    if (Array.isArray(raw)) {
      const profiles = raw.filter(isRecord).filter(v => typeof v.id === 'string' && typeof v.name === 'string' && typeof v.endpoint === 'string')
        .map(v => ({ id: String(v.id), name: String(v.name), endpoint: String(v.endpoint),
          password: v.rememberPassword === true && typeof v.password === 'string' ? v.password : '', rememberPassword: v.rememberPassword === true }));
      if (profiles.length && new Set(profiles.map(p => p.id)).size === profiles.length) return profiles;
    }
  } catch { /* Invalid or unavailable browser storage must not stop the app. */ }
  return defaults();
}
export function saveProfiles(profiles: Profile[], storage: Pick<Storage, 'setItem'> = localStorage): void {
  storage.setItem(PROFILE_KEY, JSON.stringify(profiles.map(p => ({ ...p, password: p.rememberPassword ? p.password : '' }))));
}
export function normalizeCard(value: string): string {
  const card = value.replace(/\s+/g, '').toUpperCase();
  if (!/^[0-9A-F]{16}$/.test(card)) throw new LocalizedError('error.card');
  return card;
}
export function loadCards(): SavedCard[] {
  try {
    const raw: unknown = JSON.parse(localStorage.getItem(CARD_KEY) ?? '[]');
    return Array.isArray(raw) ? raw.filter(isRecord).filter(v => typeof v.id === 'string' && typeof v.name === 'string' && typeof v.number === 'string' && /^[0-9A-F]{16}$/.test(v.number)) as unknown as SavedCard[] : [];
  } catch { return []; }
}
export function saveCards(cards: SavedCard[]): void { localStorage.setItem(CARD_KEY, JSON.stringify(cards)); }
