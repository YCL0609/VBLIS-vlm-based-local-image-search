import { Moon, Sparkles, Sun, Terminal } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { cn } from '../../utils/cn';

// One card per option; the selected one gets the indigo ring shared by the settings sections.
const OptionCard = ({ icon: Icon, label, description, selected, onClick }) => (
  <button
    type="button"
    onClick={onClick}
    aria-pressed={selected}
    className={cn(
      "flex items-center gap-2.5 p-3 rounded-xl border text-left transition-all",
      selected
        ? "bg-indigo-50 dark:bg-indigo-500/15 border-indigo-300 dark:border-indigo-500/40 text-indigo-700 dark:text-indigo-300 ring-2 ring-indigo-500/20"
        : "bg-gray-50 dark:bg-neutral-800/30 border-gray-200 dark:border-neutral-700 text-neutral-700 dark:text-neutral-300 hover:bg-gray-100 dark:hover:bg-neutral-800"
    )}
  >
    <Icon className={cn("w-5 h-5 flex-shrink-0", selected ? "text-indigo-600 dark:text-indigo-400" : "text-neutral-400")} />
    <div>
      <div className="text-xs font-bold">{label}</div>
      <div className="text-[10px] text-neutral-400 dark:text-neutral-500">{description}</div>
    </div>
  </button>
);

// Colour mode (light/dark) and UI theme style (modern/terminal). This used to live in the custom
// title bar; it now belongs to the settings the window frame does not cover.
export const ThemeStyleSection = ({ theme, setTheme, uiTheme, setUiTheme }) => {
  const { t } = useTranslation('settings');
  const colorModes = [
    { id: 'light', icon: Sun, label: t('theme.mode.light.name'), description: t('theme.mode.light.desc') },
    { id: 'dark', icon: Moon, label: t('theme.mode.dark.name'), description: t('theme.mode.dark.desc') }
  ];
  const uiThemes = [
    { id: 'modern', icon: Sparkles, label: t('theme.modern.name'), description: t('theme.modern.desc') },
    { id: 'terminal', icon: Terminal, label: t('theme.terminal.name'), description: t('theme.terminal.desc') }
  ];

  return (
    <div className="space-y-4 pt-2">
      <div>
        <label className="block text-sm font-semibold text-neutral-500 dark:text-neutral-400 mb-2">{t('theme.mode.title')}</label>
        <div className="grid grid-cols-2 gap-3">
          {colorModes.map((option) => (
            <OptionCard
              key={option.id}
              {...option}
              selected={theme === option.id}
              onClick={() => setTheme(option.id)}
            />
          ))}
        </div>
      </div>
      <div>
        <label className="block text-sm font-semibold text-neutral-500 dark:text-neutral-400 mb-2">{t('theme.title')}</label>
        <div className="grid grid-cols-2 gap-3">
          {uiThemes.map((option) => (
            <OptionCard
              key={option.id}
              {...option}
              selected={uiTheme === option.id}
              onClick={() => setUiTheme(option.id)}
            />
          ))}
        </div>
      </div>
    </div>
  );
};
