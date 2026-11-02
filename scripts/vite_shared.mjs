import react from '@vitejs/plugin-react'
import { readFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const workspace = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const packageMetadata = JSON.parse(
  readFileSync(new URL('../package.json', import.meta.url), 'utf8'),
)

export default {
  plugins: [react()],
  define: {
    __APP_VERSION__: JSON.stringify(packageMetadata.version),
  },
  root: resolve(workspace, 'src/frontend/test-web'),
  resolve: {
    alias: {
      '@decky/manifest': resolve(workspace, 'plugin.json'),
    },
  },
  server: {
    host: '127.0.0.1',
    proxy: {
      '/api': 'http://127.0.0.1:8000',
    },
  },
  build: {
    outDir: resolve(workspace, 'dist-web'),
    emptyOutDir: true,
  },
}
