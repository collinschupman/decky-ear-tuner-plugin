import { createServer } from 'vite'

import config from './vite_shared.mjs'

const apiUrl = 'http://127.0.0.1:8000/api/ear-tuner'

async function waitForApi() {
  while (true) {
    try {
      const response = await fetch(apiUrl)
      if (response.ok) return
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100))
  }
}

await waitForApi()

const server = await createServer({
  ...config,
  configFile: false,
})

await server.listen()
server.printUrls()
