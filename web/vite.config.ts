import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

/*
 * The build number, baked into the bundle at build time.
 *
 * Deliberately separate from the one /healthz reports. That one says which
 * SERVER is running; this one says which BUNDLE the browser actually loaded,
 * and the two can disagree -- a cached index.html, a CDN holding an old asset,
 * or a half-finished deploy all produce exactly that. Telling which build you
 * are looking at previously meant counting property names in minified output.
 *
 * 0 for a local build, which no published build will ever be.
 */
const BUILD_ID = Number(process.env.BUILD_ID ?? 0) || 0;

export default defineConfig({
  plugins: [react()],
  define: {
    __BUILD_ID__: JSON.stringify(BUILD_ID),
  },
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
