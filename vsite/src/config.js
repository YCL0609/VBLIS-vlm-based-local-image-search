// Where the Python API listens. The default matches `python main.py`; a user who runs the backend on
// another port (or another machine on the LAN) changes it in Settings > Connection, which stores the
// value in localStorage and reloads the page so every request picks it up.
export const DEFAULT_API_BASE = 'http://127.0.0.1:8000';

const STORAGE_KEY = 'vblis_api_base';

const stripTrailingSlash = (value) => value.replace(/\/+$/, '');

// Accepts "127.0.0.1:8001", "http://127.0.0.1:8001" or "https://host:port/base"; returns null for
// anything that is not a usable http(s) origin.
export const normalizeApiBase = (value) => {
  const trimmed = String(value ?? '').trim();
  if (!trimmed) return null;
  // "ftp://host" or "nope://" must fail instead of being read as a bare host name.
  if (trimmed.includes('://') && !/^https?:\/\//i.test(trimmed)) return null;
  const withScheme = /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`;
  try {
    const url = new URL(withScheme);
    if (!url.hostname) return null;
    const path = url.pathname === '/' ? '' : url.pathname;
    return stripTrailingSlash(`${url.protocol}//${url.host}${path}`) || null;
  } catch {
    return null;
  }
};

export const getApiBase = () => {
  try {
    const stored = normalizeApiBase(window.localStorage.getItem(STORAGE_KEY));
    if (stored) return stored;
  } catch {
    // localStorage is unavailable (private mode): fall back to the default
  }
  return DEFAULT_API_BASE;
};

export const isDefaultApiBase = () => getApiBase() === DEFAULT_API_BASE;

export const setApiBase = (value) => {
  const normalized = normalizeApiBase(value);
  if (!normalized) return null;
  try {
    window.localStorage.setItem(STORAGE_KEY, normalized);
  } catch {
    // Without storage the value cannot survive the reload the settings page triggers
  }
  return normalized;
};

export const resetApiBase = () => {
  try {
    window.localStorage.removeItem(STORAGE_KEY);
  } catch {
    // Nothing stored means the default is already in use
  }
  return DEFAULT_API_BASE;
};
