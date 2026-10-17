import assert from 'node:assert/strict';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { catalogs, errorText, initializeI18n, languages, locale, LocalizedError, msg, resolveLocale, setLocale, t, textRef, translate } from '../src/i18n';
import { normalizeCard } from '../src/storage';
import { normalizeEndpoint } from '../src/api/client';
import { useController } from '../src/useController';

const tokens = (text: string) => [...text.matchAll(/\{(\w+)\}/g)].map(m => m[1]).sort();

test('all shipped locales have complete messages and matching interpolation tokens', () => {
  const expected = Object.keys(catalogs.en).sort();
  for (const { code } of languages) {
    assert.deepEqual(Object.keys(catalogs[code]).sort(), expected);
    for (const key of expected as (keyof typeof catalogs.en)[]) {
      assert.ok(catalogs[code][key].trim(), `${code}: ${key}`);
      assert.deepEqual(tokens(catalogs[code][key]), tokens(catalogs.en[key]), `${code}: ${key}`);
    }
  }
});

test('browser locale matching handles scripts, regions and supported-language preferences', () => {
  assert.equal(resolveLocale(['zh-Hant-CN']), 'zh-TW');
  assert.equal(resolveLocale(['zh-Hans-HK']), 'zh-CN');
  for (const value of ['zh-TW', 'zh-HK', 'zh-MO', 'zh-Hant', 'zh_Hant_TW']) assert.equal(resolveLocale([value]), 'zh-TW');
  for (const value of ['zh', 'zh-CN', 'zh-SG', 'zh-Hans']) assert.equal(resolveLocale([value]), 'zh-CN');
  assert.equal(resolveLocale(['fr-FR', 'ja-JP', 'en']), 'ja');
  assert.equal(resolveLocale(['ko-KR']), 'ko');
  assert.equal(resolveLocale(['en-GB']), 'en');
  assert.equal(resolveLocale(['fr-FR']), 'en');
});

test('language preference overrides the browser and blocked storage does not prevent switching', () => {
  try {
    initializeI18n(['en'], { getItem: () => 'ko' }); assert.equal(locale.value, 'ko');
    initializeI18n(['ja-JP'], { getItem: () => null }); assert.equal(locale.value, 'ja');
    initializeI18n(['ja-JP'], { getItem: () => 'invalid' }); assert.equal(locale.value, 'ja');
    initializeI18n(['zh-TW'], { getItem: () => { throw Error('blocked'); } }); assert.equal(locale.value, 'zh-TW');
    const saved = new Map<string, string>();
    setLocale('en', { setItem: (key, value) => { saved.set(key, value); } });
    assert.equal(saved.get('arcade-room.locale.v1'), 'en');
    initializeI18n(['ja'], { getItem: key => saved.get(key) ?? null }); assert.equal(locale.value, 'en');
    setLocale('ko', { setItem: () => { throw Error('blocked'); } }); assert.equal(locale.value, 'ko');
  } finally { locale.value = 'zh-TW'; }
});

test('existing feedback, nested action labels and local errors react to a language change', () => {
  try {
    const feedback = textRef(msg('msg.actionDemo', { action: msg('action.shutdown') }));
    const error = new LocalizedError('error.card');
    const display = textRef(errorText(error));
    for (const { code } of languages) {
      locale.value = code;
      assert.equal(feedback.value, translate(code, 'msg.actionDemo', { action: msg('action.shutdown') }));
      assert.equal(display.value, translate(code, 'error.card'));
      assert.equal(error.message, translate(code, 'error.card'));
      assert.throws(() => normalizeCard('bad'), e => e instanceof Error && e.message === t('error.card'));
      assert.throws(() => normalizeEndpoint('https://example.com'), e => e instanceof Error && e.message === t('error.endpointProtocol'));
      const confirmation = t('action.confirm', { machine: '<cabinet>{action}', action: msg('action.restart'), detail: msg('action.restartDetail') });
      assert.ok(confirmation.includes('<cabinet>{action}')); // User data is not interpreted as a template.
      assert.ok(confirmation.includes(t('action.restart')));
    }
    const server = 'Unable to shutdown system (Windows error 5)';
    display.value = errorText(new Error(server)); setLocale('en'); assert.equal(display.value, server);
  } finally { locale.value = 'zh-TW'; }
});

test('demo calibration and save work in every locale and feedback can switch without reconnecting', async () => {
  const c = useController();
  try {
    await c.startDemo();
    for (const { code } of languages) {
      locale.value = code;
      await c.selectScene(4); c.updateResize({ offset_x: 17 }); await c.saveResize();
      assert.equal(c.error.value, ''); assert.equal(c.unsaved.value, false);
      assert.equal(c.resize.value?.scene, 4); assert.equal(c.currentScene.value.offset_x, 17);
      assert.ok(c.message.value.includes(t('msg.demoSaved')));
      await c.machineControl('shutdown'); assert.equal(c.connected.value, true);
      assert.equal(c.message.value, t('msg.actionDemo', { action: msg('action.shutdown') }));
    }
    locale.value = 'en'; assert.equal(c.message.value, t('msg.actionDemo', { action: msg('action.shutdown') }));
  } finally { c.disconnect(); locale.value = 'zh-TW'; }
});

test('app and client sources keep visible Chinese strings in locale catalogs', () => {
  for (const file of ['App.vue', 'useController.ts', 'storage.ts', 'api/client.ts', 'api/resize.ts', 'api/demo.ts']) {
    const source = readFileSync(new URL('../src/' + file, import.meta.url), 'utf8');
    assert.ok(!/\p{Script=Han}/u.test(source), file);
  }
});
