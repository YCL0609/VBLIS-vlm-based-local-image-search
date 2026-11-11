import { describe, it, expect, vi, afterEach } from 'vitest';
import { apiFetch } from './api';

describe('apiFetch', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  const mockFetch = () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true });
    vi.stubGlobal('fetch', fetchMock);
    return fetchMock;
  };

  it('passes a bare url through unchanged', async () => {
    const fetchMock = mockFetch();
    await apiFetch('http://x/status');
    expect(fetchMock).toHaveBeenCalledWith('http://x/status');
    expect(fetchMock.mock.calls[0]).toHaveLength(1);
  });

  it('passes the options object through unchanged', async () => {
    const fetchMock = mockFetch();
    const options = { method: 'POST' };
    await apiFetch('http://x/a', options);
    expect(fetchMock).toHaveBeenCalledWith('http://x/a', { method: 'POST' });
    expect(fetchMock.mock.calls[0][1]).toBe(options);
  });

  it('returns the fetch result', async () => {
    mockFetch();
    await expect(apiFetch('http://x/a')).resolves.toEqual({ ok: true });
  });
});
