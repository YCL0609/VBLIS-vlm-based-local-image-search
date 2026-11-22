import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Frontend-only build: the desktop shell in ../quickjs embeds dist/, so the Vite build produces
// plain files it can serve (see src/resources/list.conf).
// https://vitejs.dev/config/
export default defineConfig({
  plugins: [react()],
  build: {
    // The whole renderer is three files: index.html, index.js and index.css, side by side at the
    // root of dist (no assets/ folder, no content hashes). The shell embeds them by name.
    assetsDir: '',
    rollupOptions: {
      output: {
        entryFileNames: 'index.js',
        chunkFileNames: '[name].js',
        assetFileNames: (assetInfo) => {
          const names = assetInfo.names ?? (assetInfo.name ? [assetInfo.name] : []);
          return names.some((name) => name.endsWith('.css')) ? 'index.css' : '[name].[ext]';
        },
      },
    },
  },
})
