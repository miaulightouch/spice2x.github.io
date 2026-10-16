import { computed, ref, shallowRef } from 'vue';
import en from './locales/en.json';
import zhTW from './locales/zh-TW.json';
import zhCN from './locales/zh-CN.json';
import ja from './locales/ja.json';
import ko from './locales/ko.json';

export const languages = [
  { code: 'en', label: 'English' },
  { code: 'zh-TW', label: '繁體中文' },
  { code: 'zh-CN', label: '简体中文' },
  { code: 'ja', label: '日本語' },
  { code: 'ko', label: '한국어' },
] as const;
export type Locale = typeof languages[number]['code'];
export type MessageKey = keyof typeof en;
export type Params = Record<string, string | number | LocalizedText>;
export const catalogs: Record<Locale, Record<MessageKey, string>> = { en, 'zh-TW': zhTW, 'zh-CN': zhCN, ja, ko };
export const locale = ref<Locale>('zh-TW');
const STORAGE_KEY = 'arcade-room.locale.v1';

export function resolveLocale(preferences: readonly string[]): Locale {
  for (const preference of preferences) {
    const tag = preference.toLowerCase().replaceAll('_', '-');
    if (tag === 'zh' || tag.startsWith('zh-')) {
      if (tag.includes('hant')) return 'zh-TW';
      if (tag.includes('hans')) return 'zh-CN';
      if (/-(tw|hk|mo)(-|$)/.test(tag)) return 'zh-TW';
      return 'zh-CN';
    }
    const base = tag.split('-')[0];
    if (base === 'en' || base === 'ja' || base === 'ko') return base;
  }
  return 'en';
}
export function initializeI18n(preferences: readonly string[], storage?: Pick<Storage, 'getItem'>): void {
  let saved: string | null = null;
  try { saved = (storage ?? localStorage).getItem(STORAGE_KEY); } catch { /* Storage can be blocked. */ }
  locale.value = languages.some(l => l.code === saved) ? saved as Locale : resolveLocale(preferences);
}
export function setLocale(value: Locale, storage?: Pick<Storage, 'setItem'>): void {
  if (!languages.some(l => l.code === value)) return;
  locale.value = value;
  try { (storage ?? localStorage).setItem(STORAGE_KEY, value); } catch { /* Language switching still works. */ }
}
export function translate(language: Locale, key: MessageKey, params: Params = {}): string {
  return (catalogs[language][key] ?? en[key]).replace(/\{(\w+)\}/g, (token, name: string) => {
    const value = params[name];
    return value && typeof value === 'object' ? translate(language, value.key, value.params) : String(value ?? token);
  });
}
export function t(key: MessageKey, params?: Params): string { return translate(locale.value, key, params); }
export interface LocalizedText { key: MessageKey; params?: Params }
export type TextValue = string | LocalizedText;
export function msg(key: MessageKey, params?: Params): LocalizedText { return { key, params }; }
export function renderText(value: TextValue): string { return typeof value === 'string' ? value : t(value.key, value.params); }
/** Keep app-generated feedback reactive; server errors and user data stay verbatim. */
export function textRef(initial: TextValue = '') {
  const source = shallowRef(initial);
  return computed({ get: () => renderText(source.value), set: (value: TextValue) => { source.value = value; } });
}
export class LocalizedError extends Error {
  constructor(readonly key: MessageKey, readonly params?: Params) {
    super();
    Object.defineProperty(this, 'message', { get: () => t(this.key, this.params), configurable: true });
  }
}
export function errorText(value: unknown): TextValue {
  return value instanceof LocalizedError ? msg(value.key, value.params) : value instanceof Error ? value.message : String(value);
}
