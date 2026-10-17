import assert from 'node:assert/strict';
import { test } from 'node:test';
import { loadCards, loadProfiles, saveCards, saveProfiles, type Profile, type SavedCard } from '../src/storage';

function memoryStorage(): Pick<Storage, 'getItem' | 'setItem'> {
  const values = new Map<string, string>();
  return {
    getItem: key => values.get(key) ?? null,
    setItem: (key, value) => { values.set(key, value); },
  };
}

function withLocalStorage<T>(storage: Pick<Storage, 'getItem' | 'setItem'>, callback: () => T): T {
  const target = globalThis as typeof globalThis & { localStorage?: Storage };
  const previous = target.localStorage;
  Object.defineProperty(target, 'localStorage', { configurable: true, value: storage });
  try { return callback(); }
  finally {
    if (previous === undefined) Reflect.deleteProperty(target, 'localStorage');
    else Object.defineProperty(target, 'localStorage', { configurable: true, value: previous });
  }
}

test('profiles survive a storage round-trip beyond the old 30-entry limit', () => {
  const storage = memoryStorage();
  const profiles: Profile[] = Array.from({ length: 31 }, (_, index) => ({
    id: `profile-${index}`,
    name: `Cabinet ${index}`,
    endpoint: `ws://192.0.2.${index + 1}:1338/`,
    password: index % 2 ? `secret-${index}` : `not-saved-${index}`,
    rememberPassword: index % 2 === 1,
  }));

  saveProfiles(profiles, storage);

  assert.deepEqual(loadProfiles(storage), profiles.map(profile => ({
    ...profile,
    password: profile.rememberPassword ? profile.password : '',
  })));
});

test('cards survive a storage round-trip beyond the old 100-entry limit', () => {
  const storage = memoryStorage();
  const cards: SavedCard[] = Array.from({ length: 101 }, (_, index) => ({
    id: `card-${index}`,
    name: `Card ${index}`,
    number: `E004${index.toString(16).padStart(12, '0').toUpperCase()}`,
  }));

  withLocalStorage(storage, () => saveCards(cards));

  assert.deepEqual(withLocalStorage(storage, loadCards), cards);
});

test('storage loaders retain their existing format validation', () => {
  const profiles = memoryStorage();
  profiles.setItem('arcade-room.profiles.v1', JSON.stringify([
    { id: 'valid', name: 'Valid', endpoint: 'ws://192.0.2.1:1338/', password: 'ignored' },
    { id: 12, name: 'Invalid id', endpoint: 'ws://192.0.2.2:1338/' },
    { id: 'missing-name', endpoint: 'ws://192.0.2.3:1338/' },
  ]));
  assert.deepEqual(loadProfiles(profiles), [{
    id: 'valid', name: 'Valid', endpoint: 'ws://192.0.2.1:1338/', password: '', rememberPassword: false,
  }]);
  profiles.setItem('arcade-room.profiles.v1', '{broken');
  assert.deepEqual(loadProfiles(profiles), []);

  profiles.setItem('arcade-room.profiles.v1', JSON.stringify([]));
  assert.deepEqual(loadProfiles(profiles), []);
  profiles.setItem('arcade-room.profiles.v1', JSON.stringify({ invalid: true }));
  assert.deepEqual(loadProfiles(profiles), []);

  const cards = memoryStorage();
  cards.setItem('arcade-room.cards.v1', JSON.stringify([
    { id: 'valid', name: 'Valid', number: 'E004000000000001' },
    { id: 'bad-number', name: 'Bad number', number: 'E004' },
    { id: 'bad-shape', number: 'E004000000000002' },
  ]));
  assert.deepEqual(withLocalStorage(cards, loadCards), [{
    id: 'valid', name: 'Valid', number: 'E004000000000001',
  }]);
  cards.setItem('arcade-room.cards.v1', '{broken');
  assert.deepEqual(withLocalStorage(cards, loadCards), []);
});
