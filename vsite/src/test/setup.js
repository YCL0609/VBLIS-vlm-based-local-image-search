import { afterEach, vi } from 'vitest';
import i18n from '../i18n';

afterEach(async () => {
  vi.unstubAllEnvs();
  // A test that switched the language must not leak it into the next one.
  if (i18n.language !== 'en') await i18n.changeLanguage('en');
});
