import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { LocalFolderPickerModal } from './LocalFolderPickerModal';

const reply = (body, status = 200) => ({ ok: status < 400, status, json: async () => JSON.parse(JSON.stringify(body)) });

const HOME = {
  path: '/home/u',
  parent: '/home',
  home: '/home/u',
  separator: '/',
  truncated: false,
  skipped: 0,
  entries: [
    { name: 'Pictures', path: '/home/u/Pictures', hidden: false },
    { name: '.config', path: '/home/u/.config', hidden: true },
  ],
};

const PICTURES = {
  path: '/home/u/Pictures',
  parent: '/home/u',
  home: '/home/u',
  separator: '/',
  truncated: false,
  skipped: 0,
  entries: [{ name: '2024', path: '/home/u/Pictures/2024', hidden: false }],
};

describe('LocalFolderPickerModal', () => {
  let fetchMock;
  let onClose;
  let onSelect;

  beforeEach(() => {
    onClose = vi.fn();
    onSelect = vi.fn().mockResolvedValue(undefined);
    fetchMock = vi.fn(async (url) => {
      const parsed = new URL(String(url));
      if (parsed.pathname !== '/fs/list') throw new Error(`unexpected ${parsed.pathname}`);
      const path = parsed.searchParams.get('path');
      if (path === null || path === '/home/u') return reply(HOME);
      if (path === '/home/u/Pictures') return reply(PICTURES);
      return reply({ detail: 'Folder not found' }, 404);
    });
    vi.stubGlobal('fetch', fetchMock);
  });

  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
  });

  it('starts in the home folder, hides dotfolders and drills down', async () => {
    render(<LocalFolderPickerModal onClose={onClose} onSelect={onSelect} />);

    expect(await screen.findByRole('button', { name: 'Pictures' })).toBeTruthy();
    expect(screen.queryByRole('button', { name: '.config' })).toBeNull();

    fireEvent.click(screen.getByRole('button', { name: 'Pictures' }));
    expect(await screen.findByRole('button', { name: '2024' })).toBeTruthy();
    expect(screen.getByText('/home/u/Pictures')).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: 'Index this folder' }));
    expect(onSelect).toHaveBeenCalledWith('/home/u/Pictures');
  });

  it('shows hidden folders on request and goes back up', async () => {
    render(<LocalFolderPickerModal onClose={onClose} onSelect={onSelect} />);
    await screen.findByRole('button', { name: 'Pictures' });

    fireEvent.click(screen.getByRole('button', { name: 'Show hidden' }));
    expect(screen.getByRole('button', { name: '.config' })).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: 'Pictures' }));
    await screen.findByRole('button', { name: '2024' });
    fireEvent.click(screen.getByRole('button', { name: 'Up' }));
    expect(await screen.findByRole('button', { name: 'Pictures' })).toBeTruthy();
  });
});
