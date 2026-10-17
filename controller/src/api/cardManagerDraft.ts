import type { CardManagerState, ManagedCard } from './features';

export interface CardManagerDraft {
  cards: ManagedCard[];
  active: [string, string];
  revision?: number;
  dirty: boolean;
}

export function cloneCardManagerCards(cards: ManagedCard[]): ManagedCard[] {
  return cards.map(card => ({
    name: card.name,
    id: card.id,
    color: [card.color[0], card.color[1], card.color[2]] as [number, number, number],
  }));
}

export function mergeCardManagerDraft(draft: CardManagerDraft, server?: CardManagerState): CardManagerDraft {
  if (!server) return { cards: [], active: ['', ''], revision: undefined, dirty: false };
  const active: [string, string] = [...server.active];
  if (draft.dirty) return { ...draft, active };
  return { cards: cloneCardManagerCards(server.cards), active, revision: server.revision, dirty: false };
}
