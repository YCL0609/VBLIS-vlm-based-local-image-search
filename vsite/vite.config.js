import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Frontend-only build. The Electron shell lives in ../electron and loads dist/ (or the dev
// server) from there — it is not built by this Vite config.
// https://vitejs.dev/config/
export default defineConfig({
  plugins: [react()],
  build: {
    // The whole renderer is three files: index.html, index.js and index.css, side by side at the
    // root of dist (no assets/ folder, no content hashes). The packaged app://vblis protocol
    // serves them straight from that folder, and a stable name keeps diffs readable.
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
