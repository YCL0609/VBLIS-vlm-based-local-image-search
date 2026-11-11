import { afterEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { ThemeStyleSection } from './ThemeStyleSection';

const renderSection = (overrides = {}) => {
  const props = {
    theme: 'light',
    setTheme: vi.fn(),
    uiTheme: 'modern',
    setUiTheme: vi.fn(),
    ...overrides,
  };
  render(<ThemeStyleSection {...props} />);
  return props;
};

describe('ThemeStyleSection', () => {
  afterEach(cleanup);

  it('selects the terminal theme', () => {
    const props = renderSection();
    expect(screen.getByText('UI Theme Style')).toBeTruthy();
    fireEvent.click(screen.getByText('Retro Terminal'));
    expect(props.setUiTheme).toHaveBeenCalledWith('terminal');
  });

  it('selects the modern theme', () => {
    const props = renderSection({ uiTheme: 'terminal' });
    fireEvent.click(screen.getByText('Modern Rounded'));
    expect(props.setUiTheme).toHaveBeenCalledWith('modern');
  });

  it('highlights the active theme only', () => {
    renderSection({ uiTheme: 'terminal' });
    const active = screen.getByText('Retro Terminal').closest('button');
    const inactive = screen.getByText('Modern Rounded').closest('button');
    expect(active.className).toContain('ring-2');
    expect(inactive.className).not.toContain('ring-2');
  });

  it('selects the dark colour mode', () => {
    const props = renderSection();
    expect(screen.getByText('Colour Mode')).toBeTruthy();
    fireEvent.click(screen.getByText('Dark'));
    expect(props.setTheme).toHaveBeenCalledWith('dark');
    expect(props.setUiTheme).not.toHaveBeenCalled();
  });

  it('selects the light colour mode', () => {
    const props = renderSection({ theme: 'dark' });
    fireEvent.click(screen.getByText('Light'));
    expect(props.setTheme).toHaveBeenCalledWith('light');
  });

  it('highlights the active colour mode only', () => {
    renderSection({ theme: 'dark' });
    const active = screen.getByText('Dark').closest('button');
    const inactive = screen.getByText('Light').closest('button');
    expect(active.className).toContain('ring-2');
    expect(inactive.className).not.toContain('ring-2');
  });
});
