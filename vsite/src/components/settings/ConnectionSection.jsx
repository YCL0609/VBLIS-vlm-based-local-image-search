import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { RotateCcw } from 'lucide-react';
import { DEFAULT_API_BASE, getApiBase, normalizeApiBase, resetApiBase, setApiBase } from '../../config';

// The API address (host and port) is fixed at build time unless the user overrides it, so this
// section stores the override in localStorage and reloads: every request, including the ones made
// outside apiFetch (image sources), then starts from the new base.
export const ConnectionSection = () => {
  const { t } = useTranslation('settings');
  const [value, setValue] = useState(getApiBase());
  const [saved, setSaved] = useState(false);

  const normalized = normalizeApiBase(value);
  const isValid = Boolean(normalized);
  const isChanged = normalized ? normalized !== getApiBase() : false;

  const save = () => {
    if (!normalized) return;
    setApiBase(normalized);
    // The page reloads so hooks re-read the base; the toast would not survive it.
    window.location.reload();
  };

  const restoreDefault = () => {
    resetApiBase();
    setValue(DEFAULT_API_BASE);
    setSaved(true);
    window.location.reload();
  };

  return (
    <div className="pt-2">
      <label className="block text-sm font-semibold text-neutral-500 dark:text-neutral-400 mb-2">
        {t('connection.title')}
      </label>
      <div className="flex items-center gap-2">
        <input
          type="text"
          value={value}
          spellCheck={false}
          onChange={(e) => { setValue(e.target.value); setSaved(false); }}
          onKeyDown={(e) => { if (e.key === 'Enter') save(); }}
          placeholder={DEFAULT_API_BASE}
          aria-label={t('connection.title')}
          className="flex-1 min-w-0 px-3 py-2 rounded-xl border border-gray-200 dark:border-neutral-700 bg-gray-50 dark:bg-neutral-800/30 text-xs font-mono text-neutral-800 dark:text-neutral-200 focus:outline-none focus:ring-2 focus:ring-indigo-500/30"
        />
        <button
          type="button"
          onClick={save}
          disabled={!isValid || !isChanged}
          className="px-3 py-2 rounded-xl text-xs font-bold text-white bg-indigo-600 hover:bg-indigo-500 disabled:opacity-40 transition-colors"
        >
          {t('connection.save')}
        </button>
        <button
          type="button"
          onClick={restoreDefault}
          title={t('connection.restoreDefault')}
          className="p-2 rounded-xl text-neutral-500 hover:text-neutral-900 dark:hover:text-white hover:bg-gray-100 dark:hover:bg-neutral-800 transition-colors"
        >
          <RotateCcw className="w-4 h-4" />
        </button>
      </div>
      <p className="mt-2 text-[11px] leading-relaxed text-neutral-400">
        {isValid ? t('connection.hint') : t('connection.invalid')}
      </p>
      <p className="mt-1 text-[11px] text-neutral-400">
        {t('connection.default', { base: DEFAULT_API_BASE })}
        {saved ? ` · ${t('connection.saved')}` : ''}
      </p>
    </div>
  );
};
