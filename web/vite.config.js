import { defineConfig } from 'vite';

// The PS5 browser is an older WebKit: keep the output to ES2018 and one script.
export default defineConfig({
  base: './',
  esbuild: { jsxFactory: 'h', jsxFragment: 'Fragment', jsxInject: "import { h, Fragment } from 'preact'" },
  build: {
    target: 'es2018',
    cssTarget: 'safari12',
    assetsInlineLimit: 8192,
    modulePreload: false,
    rollupOptions: { output: { inlineDynamicImports: true, entryFileNames: 'app.js', assetFileNames: '[name][extname]' } }
  },
  server: { proxy: { '/api': 'http://127.0.0.1:8687' } }
});
