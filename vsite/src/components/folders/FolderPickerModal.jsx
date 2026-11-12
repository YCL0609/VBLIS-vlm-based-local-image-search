import { useCallback, useEffect, useState } from 'react';
import { ArrowUp, Eye, EyeOff, Folder, FolderOpen, Home, Loader2, X } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { getApiBase } from '../../config';
import { apiFetch } from '../../utils/api';
import { useBackdropDismiss } from '../../hooks/useBackdropDismiss';

// The web UI cannot learn an absolute folder path from a file picker or a drop, so the API lists
// directories for us (GET /fs/list). The user navigates down/up and confirms the current folder.
export const FolderPickerModal = ({ isOpen, onClose, onSelect, uiTheme = 'modern' }) => {
  const { t } = useTranslation('folders');
  const backdropDismiss = useBackdropDismiss(onClose);
  const [currentPath, setCurrentPath] = useState(null);
  const [listing, setListing] = useState(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState(null);
  const [showHidden, setShowHidden] = useState(false);

  const load = useCallback(async (path) => {
    setIsLoading(true);
    setError(null);
    try {
      const params = new URLSearchParams();
      if (path) params.set('path', path);
      const res = await apiFetch(`${getApiBase()}/fs/list?${params.toString()}`);
      const data = await res.json().catch(() => null);
      if (!res.ok) {
        throw new Error(typeof data?.detail === 'string' ? data.detail : t('picker.loadFailed'));
      }
      setListing(data);
      setCurrentPath(data.path);
    } catch (err) {
      setError(err?.message || t('picker.loadFailed'));
    } finally {
      setIsLoading(false);
    }
  }, [t]);

  // Every open starts at the home folder, not at whatever was browsed last time.
  useEffect(() => {
    if (!isOpen) return;
    setListing(null);
    setCurrentPath(null);
    setError(null);
    load(null);
  }, [isOpen, load]);

  if (!isOpen) return null;
  const isModern = uiTheme === 'modern';
  const allEntries = listing?.entries || [];
  const entries = showHidden ? allEntries : allEntries.filter(entry => !entry.hidden);

  return (
    <div className="fixed inset-0 z-[70] flex items-center justify-center p-4 bg-black/60 backdrop-blur-md animate-in fade-in duration-150" {...backdropDismiss}>
      <div
        className={`w-full max-w-xl flex flex-col max-h-[85vh] rounded-3xl shadow-2xl border transition-all animate-in zoom-in-95 duration-150 ${
          isModern
            ? 'bg-white dark:bg-neutral-900 border-gray-200 dark:border-neutral-800 text-neutral-900 dark:text-white'
            : 'bg-black border-2 border-white text-white font-mono'
        }`}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between px-6 pt-5 pb-3">
          <div className="flex items-center gap-2.5">
            <div className={`p-2 rounded-xl border ${isModern ? 'bg-indigo-50 dark:bg-indigo-950/50 text-indigo-600 dark:text-indigo-400 border-indigo-200 dark:border-indigo-500/30' : 'border-white'}`}>
              <FolderOpen className="w-5 h-5" />
            </div>
            <div>
              <h3 className="text-sm font-bold">{t('picker.title')}</h3>
              <p className="text-[11px] text-neutral-400">{t('picker.subtitle')}</p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-2 rounded-xl text-neutral-400 hover:text-neutral-900 dark:hover:text-white hover:bg-black/5 dark:hover:bg-white/10 transition-colors"
            title={t('picker.cancel')}
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="px-6 pb-2 flex items-center gap-2">
          <button
            type="button"
            onClick={() => load(listing?.home)}
            className={`flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg text-xs font-semibold transition-colors ${isModern ? 'bg-gray-100 dark:bg-neutral-800 hover:bg-gray-200 dark:hover:bg-neutral-700' : 'border border-white hover:bg-white hover:text-black'}`}
          >
            <Home className="w-3.5 h-3.5" /> {t('picker.home')}
          </button>
          <button
            type="button"
            onClick={() => listing?.parent && load(listing.parent)}
            disabled={!listing?.parent || isLoading}
            className={`flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg text-xs font-semibold transition-colors disabled:opacity-40 ${isModern ? 'bg-gray-100 dark:bg-neutral-800 hover:bg-gray-200 dark:hover:bg-neutral-700' : 'border border-white hover:bg-white hover:text-black'}`}
          >
            <ArrowUp className="w-3.5 h-3.5" /> {t('picker.up')}
          </button>
          <button
            type="button"
            onClick={() => setShowHidden(v => !v)}
            aria-pressed={showHidden}
            className={`ml-auto flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg text-xs font-semibold transition-colors ${isModern ? 'bg-gray-100 dark:bg-neutral-800 hover:bg-gray-200 dark:hover:bg-neutral-700' : 'border border-white hover:bg-white hover:text-black'}`}
          >
            {showHidden ? <EyeOff className="w-3.5 h-3.5" /> : <Eye className="w-3.5 h-3.5" />}
            {showHidden ? t('picker.hideHidden') : t('picker.showHidden')}
          </button>
        </div>

        <div className={`mx-6 mb-2 px-3 py-2 rounded-lg text-[11px] break-all ${isModern ? 'bg-gray-50 dark:bg-neutral-800/60 text-neutral-500 dark:text-neutral-400' : 'border border-white text-white'}`}>
          {currentPath || '…'}
        </div>

        <div className="flex-1 min-h-[180px] overflow-y-auto custom-scrollbar mx-6 mb-3 border rounded-xl border-gray-100 dark:border-neutral-800">
          {isLoading && (
            <div className="flex items-center justify-center gap-2 py-10 text-xs text-neutral-400">
              <Loader2 className="w-4 h-4 animate-spin" /> {t('picker.loading')}
            </div>
          )}
          {!isLoading && error && (
            <div className="px-4 py-6 text-xs text-rose-500">{error}</div>
          )}
          {!isLoading && !error && entries.length === 0 && (
            <div className="px-4 py-6 text-xs text-neutral-400">{t('picker.empty')}</div>
          )}
          {!isLoading && !error && entries.map(entry => (
            <button
              key={entry.path}
              type="button"
              onClick={() => load(entry.path)}
              className={`w-full flex items-center gap-2 px-4 py-2 text-left text-xs transition-colors ${isModern ? 'hover:bg-gray-50 dark:hover:bg-neutral-800' : 'hover:bg-white hover:text-black'} ${entry.hidden ? 'opacity-60' : ''}`}
            >
              <Folder className="w-4 h-4 text-amber-500 shrink-0" />
              <span className="truncate">{entry.name}</span>
            </button>
          ))}
          {listing?.truncated && (
            <div className="px-4 py-2 text-[11px] text-neutral-400">{t('picker.truncated')}</div>
          )}
        </div>

        <div className="flex items-center justify-between gap-3 px-6 pb-5">
          <p className="text-[11px] text-neutral-400 truncate">{t('picker.hint')}</p>
          <div className="flex items-center gap-2 shrink-0">
            <button
              type="button"
              onClick={onClose}
              className={`px-3 py-2 rounded-xl text-xs font-bold transition-colors ${isModern ? 'bg-gray-100 dark:bg-neutral-800 hover:bg-gray-200 dark:hover:bg-neutral-700' : 'border border-white hover:bg-white hover:text-black'}`}
            >
              {t('picker.cancel')}
            </button>
            <button
              type="button"
              disabled={!currentPath || isLoading}
              onClick={() => currentPath && onSelect(currentPath)}
              className="px-3 py-2 rounded-xl text-xs font-bold text-white bg-indigo-600 hover:bg-indigo-500 disabled:opacity-40 transition-colors"
            >
              {t('picker.selectThis')}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};
