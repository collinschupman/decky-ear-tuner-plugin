import { cp, mkdir, readdir, rm } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const root = fileURLToPath(new URL('../', import.meta.url))
const source = path.join(root, 'src', 'ear_tuner')
const destination = path.join(root, 'ear_tuner')
const modules = (await readdir(source)).filter((name) => name.endsWith('.py'))
if (
  !modules.includes('__init__.py') ||
  !modules.includes('application_service.py')
) {
  throw new Error('Ear Tuner Python runtime sources are incomplete')
}
await rm(destination, { recursive: true, force: true })
await mkdir(destination)
for (const module of modules) {
  await cp(path.join(source, module), path.join(destination, module))
}
