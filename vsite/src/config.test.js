import { beforeEach, describe, expect, it } from 'vitest';
import {
  DEFAULT_API_BASE,
  getApiBase,
  isDefaultApiBase,
  normalizeApiBase,
  resetApiBase,
  setApiBase,
} from './config';

describe('normalizeApiBase', () => {
  it('keeps a full http origin unchanged', () => {
    expect(normalizeApiBase('http://127.0.0.1:8000')).toBe('http://127.0.0.1:8000');
  });

  it('adds the http scheme to a bare host and port', () => {
    expect(normalizeApiBase('127.0.0.1:8001')).toBe('http://127.0.0.1:8001');
  });

  it('strips a trailing slash', () => {
    expect(normalizeApiBase('http://127.0.0.1:8000/')).toBe('http://127.0.0.1:8000');
  });

  it('keeps a path prefix without a trailing slash', () => {
    expect(normalizeApiBase('https://example.test:8443/api/')).toBe('https://example.test:8443/api');
  });

  it('rejects empty and malformed values', () => {
    expect(normalizeApiBase('')).toBeNull();
    expect(normalizeApiBase('   ')).toBeNull();
    expect(normalizeApiBase(null)).toBeNull();
    expect(normalizeApiBase('http://')).toBeNull();
  });
});

describe('api base preference', () => {
  beforeEach(() => {
    window.localStorage.clear();
  });

  it('falls back to the default until something is saved', () => {
    expect(getApiBase()).toBe(DEFAULT_API_BASE);
    expect(isDefaultApiBase()).toBe(true);
  });

  it('reads back a saved base', () => {
    expect(setApiBase('127.0.0.1:8001')).toBe('http://127.0.0.1:8001');
    expect(getApiBase()).toBe('http://127.0.0.1:8001');
    expect(isDefaultApiBase()).toBe(false);
  });

  it('does not store an invalid value', () => {
    expect(setApiBase('nope://')).toBeNull();
    expect(getApiBase()).toBe(DEFAULT_API_BASE);
  });

  it('restores the default', () => {
    setApiBase('127.0.0.1:8001');
    expect(resetApiBase()).toBe(DEFAULT_API_BASE);
    expect(getApiBase()).toBe(DEFAULT_API_BASE);
  });
});
