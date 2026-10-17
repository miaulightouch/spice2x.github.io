import { isRecord } from './client';
import { LocalizedError } from '../i18n';

export type OptionType = 'bool' | 'text' | 'integer' | 'hex' | 'enum';

export interface GameList {
  current: string;
  games: string[];
}

export interface OptionElement {
  value: string;
  label: string;
}

export function parseOptionChoices(data: unknown[], name: string, picker?: string): OptionElement[] {
  const raw = data[0];
  if (!isRecord(raw) || raw.name !== name || !Array.isArray(raw.choices)) return fail('Invalid option choices');
  return raw.choices.map(choice => {
    if (!isRecord(choice)) return fail('Invalid option choice');
    const value = stringValue(choice.value, 'choice.value');
    if (picker === 'cpu_affinity') {
      if (!/^0x[0-9a-f]{1,16}$/i.test(value)) return fail('Invalid CPU choice');
      const mask = BigInt(value);
      if (!mask || (mask & (mask - 1n)) !== 0n) return fail('Invalid CPU choice');
    }
    return { value, label: stringValue(choice.label, 'choice.label') };
  });
}

export interface OptionField {
  name: string;
  title: string;
  description: string;
  category: string;
  type: OptionType;
  value: string;
  sensitive: boolean;
  has_value: boolean;
  hidden: boolean;
  disabled: boolean;
  elements: OptionElement[];
  display_name?: string;
  setting_name?: string;
  quick_setting_category?: string;
  picker?: string;
  file_extension?: string;
}

export interface OptionsState {
  game: string;
  revision: number;
  restart_required: boolean;
  options: OptionField[];
  quick_categories?: string[];
}

export type PatchType = 'memory' | 'signature' | 'union' | 'integer';
export type PatchStatus = 'enabled' | 'disabled' | 'error';

export interface PatchEntry {
  id: string;
  name: string;
  description: string;
  caution: string;
  group: string;
  game_code?: string;
  type: PatchType;
  enabled: boolean;
  status: PatchStatus;
  error: string;
  unverified: boolean;
  choices: string[];
  selected: string;
  min?: number;
  max?: number;
  value?: number;
}

export interface PatchState {
  game: string;
  auto_apply: boolean;
  dirty: boolean;
  standalone: boolean;
  can_import: boolean;
  import_url: string;
  recent_urls: string[];
  import_errors: string;
  patches: PatchEntry[];
  identity?: { game_identifier: string; pe_identifier: string; patch_file: string };
  groups?: Array<{ game_code: string; id: string; name: string; description: string; caution: string }>;
}

export function groupPatches(state: PatchState | undefined, search: string, sort: string) {
  const query = search.trim().toLowerCase();
  const groups = new Map<string, PatchEntry[]>();
  const metadata = new Map(state?.groups?.map(group => [JSON.stringify([group.game_code, group.id]), group]));
  const sorted = [...(state?.patches ?? [])].sort((a, b) => sort === 'status'
    ? a.status.localeCompare(b.status) || a.name.localeCompare(b.name) : a.name.localeCompare(b.name));
  for (const patch of sorted) {
    const key = JSON.stringify([patch.game_code ?? '', patch.group]);
    const items = groups.get(key) ?? []; items.push(patch); groups.set(key, items);
  }
  return [...groups].flatMap(([key, members]) => {
    const { group: id, game_code = '' } = members[0]!;
    const group = metadata.get(key);
    const matches = (values: string[]) => !query || values.some(value => value.toLowerCase().includes(query));
    const groupMatches = group && matches([group.name, group.description, group.caution]);
    const items = groupMatches ? members : members.filter(patch => matches([patch.id, patch.name, patch.group, patch.description, patch.caution]));
    if (!items.length) return [];
    const hasError = members.some(patch => patch.status === 'error');
    return [{ key, id, game_code, items, metadata: group, hasError,
      checked: members.every(patch => patch.status === 'enabled'),
      mixed: !hasError && members.some(patch => patch.status === 'enabled') && members.some(patch => patch.status === 'disabled') }];
  });
}

export function normalizePatchUrl(value: string): string {
  const text = value.trim();
  try {
    if (/[\u0000-\u0020\u007f]/.test(text)) throw new Error();
    const url = new URL(text);
    if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password ||
        url.href.includes('?') || url.href.includes('#') || url.href.length > 4096) throw new Error();
    return url.href;
  } catch { throw new LocalizedError('error.patchUrl'); }
}

export interface ManagedCard {
  name: string;
  id: string;
  color: [number, number, number];
}

export interface CardManagerState {
  revision: number;
  cards: ManagedCard[];
  active: [string, string];
  read_only?: Array<{ name: string; id: string; source: string }>;
}

export interface KeypadCapabilities {
  model: string;
  keypad_count: number;
  keys: boolean;
  insert_card: boolean;
}

export function parseKeypadCapabilities(data: unknown[]): KeypadCapabilities {
  const value = data[0];
  if (!isRecord(value)) return fail('Invalid keypad capabilities');
  const count = integerValue(value.keypad_count, 'keypad_count');
  if (count < 0 || count > 2) return fail('Invalid keypad count');
  return { model: stringValue(value.model, 'model'), keypad_count: count,
    keys: boolValue(value.keys, 'keys'), insert_card: boolValue(value.insert_card, 'insert_card') };
}

export interface IoButton {
  name: string;
  value: number;
  overridden: boolean;
}

export interface IoAnalog {
  name: string;
  value: number;
  overridden: boolean;
}

export interface IoLight {
  name: string;
  value: number;
  overridden: boolean;
}

export interface IoSnapshot {
  buttons: IoButton[];
  analogs: IoAnalog[];
  lights: IoLight[];
}

export function generateCardId(): string {
  const bytes = new Uint8Array(4);
  const cryptoApi = globalThis.crypto;
  if (cryptoApi?.getRandomValues) cryptoApi.getRandomValues(bytes);
  else for (let index = 0; index < bytes.length; index++) bytes[index] = Math.floor(Math.random() * 256);
  return 'E0040100' + [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('').toUpperCase();
}

function fail(message: string): never {
  throw new Error(message);
}

function stringValue(value: unknown, field: string): string {
  return typeof value === 'string' ? value : fail(`Invalid ${field}`);
}

function boolValue(value: unknown, field: string): boolean {
  return typeof value === 'boolean' ? value : fail(`Invalid ${field}`);
}

function numberValue(value: unknown, field: string): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fail(`Invalid ${field}`);
}

function integerValue(value: unknown, field: string): number {
  const number = numberValue(value, field);
  return Number.isSafeInteger(number) ? number : fail(`Invalid ${field}`);
}

function revisionValue(value: unknown, field: string): number {
  const number = integerValue(value, field);
  return number >= 0 ? number : fail(`Invalid ${field}`);
}

function patchIntegerValue(value: unknown, field: string): number {
  const number = integerValue(value, field);
  return number >= -2147483648 && number <= 2147483647 ? number : fail(`Invalid ${field}`);
}

function optionalPatchInteger(value: unknown, field: string): number | undefined {
  return value === undefined ? undefined : patchIntegerValue(value, field);
}

function arrayValue(value: unknown, field: string): unknown[] {
  return Array.isArray(value) ? value : fail(`Invalid ${field}`);
}

export function parseGames(data: unknown[]): GameList {
  const value = data[0];
  if (!isRecord(value)) return fail('Invalid game list');
  const games = arrayValue(value.games, 'games').map((game, index) => stringValue(game, `games[${index}]`));
  return { current: stringValue(value.current, 'current'), games };
}

const OPTION_TYPES: OptionType[] = ['bool', 'text', 'integer', 'hex', 'enum'];

export function parseOptions(data: unknown[]): OptionsState {
  const value = data[0];
  if (!isRecord(value) || !Array.isArray(value.options)) return fail('Invalid options response');
  return {
    game: stringValue(value.game, 'game'),
    revision: revisionValue(value.revision, 'revision'),
    restart_required: boolValue(value.restart_required, 'restart_required'),
    quick_categories: value.quick_categories === undefined ? [] : arrayValue(value.quick_categories, 'quick_categories').map(item => stringValue(item, 'quick_category')),
    options: value.options.map((raw, index) => {
      if (!isRecord(raw) || !OPTION_TYPES.includes(raw.type as OptionType)) return fail(`Invalid option ${index}`);
      return {
        name: stringValue(raw.name, `options[${index}].name`),
        title: stringValue(raw.title, `options[${index}].title`),
        description: stringValue(raw.description, `options[${index}].description`),
        category: stringValue(raw.category, `options[${index}].category`),
        type: raw.type as OptionType,
        value: stringValue(raw.value, `options[${index}].value`),
        sensitive: boolValue(raw.sensitive, `options[${index}].sensitive`),
        has_value: boolValue(raw.has_value, `options[${index}].has_value`),
        hidden: boolValue(raw.hidden, `options[${index}].hidden`),
        disabled: boolValue(raw.disabled, `options[${index}].disabled`),
        elements: arrayValue(raw.elements, `options[${index}].elements`).map((item, elementIndex) => {
          if (!isRecord(item)) return fail(`Invalid option element ${index}/${elementIndex}`);
          return {
            value: stringValue(item.value, `options[${index}].elements[${elementIndex}].value`),
            label: stringValue(item.label, `options[${index}].elements[${elementIndex}].label`),
          };
        }),
        display_name: raw.display_name === undefined ? '' : stringValue(raw.display_name, 'display_name'),
        setting_name: raw.setting_name === undefined ? '' : stringValue(raw.setting_name, 'setting_name'),
        quick_setting_category: raw.quick_setting_category === undefined ? '' : stringValue(raw.quick_setting_category, 'quick_setting_category'),
        picker: raw.picker === undefined ? 'none' : stringValue(raw.picker, 'picker'),
        file_extension: raw.file_extension === undefined ? '' : stringValue(raw.file_extension, 'file_extension'),
      };
    }),
  };
}

const PATCH_TYPES: PatchType[] = ['memory', 'signature', 'union', 'integer'];
const PATCH_STATUS: PatchStatus[] = ['enabled', 'disabled', 'error'];

export function parsePatches(data: unknown[]): PatchState {
  const value = data[0];
  if (!isRecord(value) || !Array.isArray(value.patches)) return fail('Invalid patches response');
  return {
    game: stringValue(value.game, 'game'),
    auto_apply: boolValue(value.auto_apply, 'auto_apply'),
    dirty: boolValue(value.dirty, 'dirty'),
    standalone: boolValue(value.standalone, 'standalone'),
    can_import: value.can_import === undefined ? false : boolValue(value.can_import, 'can_import'),
    import_url: value.import_url === undefined ? '' : stringValue(value.import_url, 'import_url'),
    recent_urls: value.recent_urls === undefined ? [] : arrayValue(value.recent_urls, 'recent_urls').map((url, index) => stringValue(url, `recent_urls[${index}]`)),
    import_errors: value.import_errors === undefined ? '' : stringValue(value.import_errors, 'import_errors'),
    ...(value.identity === undefined ? {} : { identity: parsePatchIdentity(value.identity) }),
    groups: value.groups === undefined ? [] : arrayValue(value.groups, 'groups').map(raw => {
      if (!isRecord(raw)) return fail('Invalid patch group');
      return { game_code: stringValue(raw.game_code, 'group.game_code'), id: stringValue(raw.id, 'group.id'), name: stringValue(raw.name, 'group.name'), description: stringValue(raw.description, 'group.description'),
        caution: stringValue(raw.caution, 'group.caution') };
    }),
    patches: value.patches.map((raw, index) => {
      if (!isRecord(raw) || !PATCH_TYPES.includes(raw.type as PatchType) || !PATCH_STATUS.includes(raw.status as PatchStatus)) {
        return fail(`Invalid patch ${index}`);
      }
      const patch = {
        id: stringValue(raw.id, `patches[${index}].id`),
        name: stringValue(raw.name, `patches[${index}].name`),
        description: stringValue(raw.description, `patches[${index}].description`),
        caution: stringValue(raw.caution, `patches[${index}].caution`),
        group: stringValue(raw.group, `patches[${index}].group`),
        game_code: raw.game_code === undefined ? '' : stringValue(raw.game_code, `patches[${index}].game_code`),
        type: raw.type as PatchType,
        enabled: boolValue(raw.enabled, `patches[${index}].enabled`),
        status: raw.status as PatchStatus,
        error: stringValue(raw.error, `patches[${index}].error`),
        unverified: boolValue(raw.unverified, `patches[${index}].unverified`),
        choices: arrayValue(raw.choices, `patches[${index}].choices`).map((choice, choiceIndex) =>
          stringValue(choice, `patches[${index}].choices[${choiceIndex}]`)),
        selected: stringValue(raw.selected, `patches[${index}].selected`),
        min: optionalPatchInteger(raw.min, `patches[${index}].min`),
        max: optionalPatchInteger(raw.max, `patches[${index}].max`),
        value: optionalPatchInteger(raw.value, `patches[${index}].value`),
      };
      if (patch.min !== undefined && patch.max !== undefined && patch.min > patch.max) return fail(`Invalid patch range ${index}`);
      if (!patch.unverified && patch.status !== 'error' && patch.value !== undefined && ((patch.min !== undefined && patch.value < patch.min) || (patch.max !== undefined && patch.value > patch.max))) {
        return fail(`Invalid patch value ${index}`);
      }
      return patch;
    }),
  };
}

function parsePatchIdentity(raw: unknown): NonNullable<PatchState['identity']> {
  if (!isRecord(raw)) return fail('Invalid patch identity');
  return { game_identifier: stringValue(raw.game_identifier, 'game_identifier'),
    pe_identifier: stringValue(raw.pe_identifier, 'pe_identifier'), patch_file: stringValue(raw.patch_file, 'patch_file') };
}

function parseColor(value: unknown, field: string): [number, number, number] {
  const color = arrayValue(value, field).map((component, index) => numberValue(component, `${field}[${index}]`));
  if (color.length !== 3 || color.some(component => component < 0 || component > 1)) {
    return fail(`Invalid ${field}`);
  }
  return [color[0]!, color[1]!, color[2]!];
}

export function parseCardManager(data: unknown[]): CardManagerState {
  const value = data[0];
  if (!isRecord(value) || !Array.isArray(value.cards) || !Array.isArray(value.active)) return fail('Invalid card manager response');
  if (value.active.length !== 2) return fail('Invalid active card list');
  const active = value.active.map((raw, index) => {
    const id = stringValue(raw, `active[${index}]`).toUpperCase();
    return id === '' || /^[0-9A-F]{16}$/.test(id) ? id : fail(`Invalid active card ${index}`);
  }) as [string, string];
  return {
    revision: revisionValue(value.revision, 'revision'),
    cards: value.cards.map((raw, index) => {
      if (!isRecord(raw)) return fail(`Invalid card ${index}`);
      const id = stringValue(raw.id, `cards[${index}].id`).toUpperCase();
      if (!/^[0-9A-F]{16}$/.test(id)) return fail(`Invalid card id ${index}`);
      return { name: stringValue(raw.name, `cards[${index}].name`), id, color: parseColor(raw.color, `cards[${index}].color`) };
    }),
    active,
    read_only: value.read_only === undefined ? [] : arrayValue(value.read_only, 'read_only').map(raw => {
      if (!isRecord(raw)) return fail('Invalid read-only card');
      const id = stringValue(raw.id, 'read_only.id').toUpperCase();
      if (!/^[0-9A-F]{16}$/.test(id)) return fail('Invalid read-only card id');
      return { id, name: stringValue(raw.name, 'read_only.name'), source: stringValue(raw.source, 'read_only.source') };
    }),
  };
}

function parseIoRows(data: unknown[], kind: 'button' | 'analog' | 'light'): Array<IoButton | IoAnalog | IoLight> {
  return data.map((raw, index) => {
    if (!Array.isArray(raw) || raw.length < 3) return fail(`Invalid ${kind} row ${index}`);
    const value = numberValue(raw[1], `${kind}[${index}].value`);
    if (value < 0 || value > 1) return fail(`Invalid ${kind}[${index}].value`);
    return {
      name: stringValue(raw[0], `${kind}[${index}].name`),
      value,
      overridden: boolValue(raw[2], `${kind}[${index}].overridden`),
    };
  });
}

export function parseIoButtons(data: unknown[]): IoButton[] { return parseIoRows(data, 'button') as IoButton[]; }
export function parseIoAnalogs(data: unknown[]): IoAnalog[] { return parseIoRows(data, 'analog') as IoAnalog[]; }
export function parseIoLights(data: unknown[]): IoLight[] { return parseIoRows(data, 'light') as IoLight[]; }
