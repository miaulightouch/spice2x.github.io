import type { OptionField, OptionsState } from './features';

export interface OptionDraftState {
  game: string;
  revision?: number;
  values: Record<string, string>;
  touched: Record<string, boolean>;
}

export function isOptionDraftDirty(state: OptionDraftState): boolean {
  return Object.values(state.touched).some(Boolean);
}

export function clearOptionDraft(state: OptionDraftState): void {
  for (const name of Object.keys(state.values)) delete state.values[name];
  for (const name of Object.keys(state.touched)) delete state.touched[name];
  state.game = '';
  state.revision = undefined;
}

/** Merge a server snapshot without rebasing touched fields or their revision. */
export function mergeOptionDraft(state: OptionDraftState, server?: OptionsState): boolean {
  if (!server) {
    clearOptionDraft(state);
    return true;
  }
  if (state.game && state.game !== server.game) {
    if (isOptionDraftDirty(state)) return false;
    clearOptionDraft(state);
  }
  state.game = server.game;
  if (!isOptionDraftDirty(state)) state.revision = server.revision;
  for (const option of server.options) {
    if (!state.touched[option.name]) state.values[option.name] = option.value;
    if (state.touched[option.name] === undefined) state.touched[option.name] = false;
  }
  return true;
}

export function setOptionDraftValue(state: OptionDraftState, name: string, value: string): void {
  state.values[name] = value;
  state.touched[name] = true;
}

export function optionDraftChanges(state: OptionDraftState, options: OptionField[], serverGame = state.game): Array<{ name: string; value: string }> {
  if (state.game !== serverGame) return [];
  return options.filter(option => state.touched[option.name] && state.values[option.name] !== undefined)
    .map(option => ({ name: option.name, value: state.values[option.name]! }));
}

export function clearOptionDraftTouches(state: OptionDraftState, changes: Array<{ name: string }>): void {
  for (const change of changes) state.touched[change.name] = false;
}

export function prepareOptionGameChange(state: OptionDraftState, requestedGame: string, confirmDiscard: () => boolean): { accepted: boolean; game: string } {
  if (state.game && state.game !== requestedGame && isOptionDraftDirty(state)) {
    if (!confirmDiscard()) return { accepted: false, game: state.game };
    clearOptionDraft(state);
  }
  return { accepted: true, game: requestedGame };
}

export function validOptionValue(option: OptionField, value: string): boolean {
  if (!value) return true;
  if (value.length > 4096 || value.includes('\0')) return false;
  if (option.type === 'integer') return /^\d+$/.test(value) && Number(value) <= 0xffffffff;
  if (option.type === 'hex') {
    if (!/^(?:0x)?[0-9a-f]+$/i.test(value)) return false;
    return BigInt(/^0x/i.test(value) ? value : '0x' + value) <= 0xffffffffffffffffn;
  }
  if (option.type === 'bool') return value === '/ENABLED';
  if (option.type === 'enum') return option.elements.some(element => element.value === value);
  return true;
}
