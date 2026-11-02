import deckyPlugin from '@decky/rollup'
import importAssets from 'rollup-plugin-import-assets'
import { readFileSync } from 'node:fs'

const packageMetadata = JSON.parse(
  readFileSync(new URL('./package.json', import.meta.url), 'utf8'),
)

const injectAppVersion = {
  name: 'inject-app-version',
  transform(source) {
    if (!source.includes('__APP_VERSION__')) return null
    return {
      code: source.replaceAll(
        '__APP_VERSION__',
        JSON.stringify(packageMetadata.version),
      ),
      map: null,
    }
  },
}

const cssAsString = {
  name: 'css-as-string',
  transform(source, id) {
    if (!id.endsWith('.css')) return null
    return { code: `export default ${JSON.stringify(source)}`, map: null }
  },
}

const config = deckyPlugin({
  plugins: [
    injectAppVersion,
    cssAsString,
    importAssets({
      include: [/\.wav$/i],
      publicPath: 'http://127.0.0.1:1337/plugins/Ear Tuner/',
    }),
  ],
})

config.input = './src/frontend/index.tsx'

export default config
