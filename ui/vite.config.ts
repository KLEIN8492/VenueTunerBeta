import { defineConfig } from 'vite';
import { resolve } from 'node:path';

export default defineConfig({
  base: './', // loaded from file:// by Electron
  build: {
    outDir: 'dist',
    rollupOptions: {
      input: {
        measurement: resolve(__dirname, 'measurement.html'),
        live: resolve(__dirname, 'live.html'),
        settings: resolve(__dirname, 'settings.html'),
      },
    },
  },
});
