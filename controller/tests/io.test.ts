import assert from 'node:assert/strict';
import { test } from 'node:test';
import { validateIoPulse } from '../src/api/io';

test('accepts the complete native pulse range', () => {
  assert.equal(validateIoPulse(0, 1), true);
  assert.equal(validateIoPulse(1, 2000), true);
  assert.equal(validateIoPulse(0.375, 180), true);
});

test('rejects values outside the unit interval or non-finite values', () => {
  assert.equal(validateIoPulse(-0.01, 180), false);
  assert.equal(validateIoPulse(1.01, 180), false);
  assert.equal(validateIoPulse(Number.NaN, 180), false);
  assert.equal(validateIoPulse(Number.POSITIVE_INFINITY, 180), false);
});

test('rejects durations outside the integer millisecond contract', () => {
  assert.equal(validateIoPulse(0.5, 0), false);
  assert.equal(validateIoPulse(0.5, 2001), false);
  assert.equal(validateIoPulse(0.5, 1.5), false);
  assert.equal(validateIoPulse(0.5, Number.NaN), false);
});
