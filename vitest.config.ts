import { defineConfig } from 'vitest/config'
import viteConfig from './scripts/vite_shared.mjs'

export default defineConfig({
  ...viteConfig,
  root: process.cwd(),
  test: {
    environment: 'jsdom',
    environmentOptions: {
      jsdom: {
        url: 'http://localhost/',
      },
    },
    restoreMocks: true,
    coverage: {
      include: ['src/frontend/decky/app.tsx'],
      provider: 'v8',
      reporter: ['text'],
    },
  },
})
