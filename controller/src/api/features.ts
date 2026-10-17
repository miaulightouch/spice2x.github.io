import { isRecord } from './client';

export type OptionType = 'bool' | 'text' | 'integer' | 'hex' | 'enum';

export interface GameList {
  current: string;
  games: string[];
}

export interface OptionElement {
  value: string;
  label: string;
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
}

export interface OptionsState {
  game: string;
  revision: number;
  restart_required: boolean;
  options: OptionField[];
}

export type PatchType = 'memory' | 'signature' | 'union' | 'integer';
export type PatchStatus = 'enabled' | 'disabled' | 'error';

export interface PatchEntry {
  id: string;
  name: string;
  description: string;
  caution: string;
  group: string;
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
  patches: PatchEntry[];
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
  const bytes = new Uint8Array(8);
  const cryptoApi = globalThis.crypto;
  if (cryptoApi?.getRandomValues) cryptoApi.getRandomValues(bytes);
  else for (let index = 0; index < bytes.length; index++) bytes[index] = Math.floor(Math.random() * 256);
  return [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('').toUpperCase();
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
      if (patch.value !== undefined && ((patch.min !== undefined && patch.value < patch.min) || (patch.max !== undefined && patch.value > patch.max))) {
        return fail(`Invalid patch value ${index}`);
      }
      return patch;
    }),
  };
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
