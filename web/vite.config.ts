import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    // During `npm run dev` the API lives on the server process, not Vite.
    proxy: {
      '/api': { target: 'http://127.0.0.1:8788', changeOrigin: true, ws: true },
      '/healthz': { target: 'http://127.0.0.1:8788', changeOrigin: true },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
    // Hashed filenames let the server mark them immutable.
    rollupOptions: {
      output: {
        entryFileNames: 'assets/[name]-[hash].js',
        chunkFileNames: 'assets/[name]-[hash].js',
        assetFileNames: 'assets/[name]-[hash].[ext]',
      },
    },
  },
});
