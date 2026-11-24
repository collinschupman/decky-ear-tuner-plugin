import { cleanup, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, expect, it, vi } from 'vitest'

import { AudioServiceControl } from './audio_service_control'

afterEach(() => {
  cleanup()
  vi.unstubAllGlobals()
})

it('stops and restarts audio while keeping the desktop page open', async () => {
  let running = true
  const fetchMock = vi.fn(async (_url: string, init?: RequestInit) => {
    if (init?.method === 'POST') running = JSON.parse(String(init.body)).running
    return { ok: true, status: 200, json: async () => ({ running }) }
  })
  vi.stubGlobal('fetch', fetchMock)
  const user = userEvent.setup()
  render(
    <AudioServiceControl>
      <div>Audio settings</div>
    </AudioServiceControl>,
  )

  await user.click(await screen.findByRole('button', { name: 'Stop Audio' }))
  expect(
    await screen.findByRole('button', { name: 'Start Audio' }),
  ).toBeTruthy()
  expect(screen.queryByText('Audio settings')).toBeNull()
  expect(fetchMock).toHaveBeenCalledWith(
    '/api/audio-service',
    expect.objectContaining({ method: 'POST', body: '{"running":false}' }),
  )
  await user.click(screen.getByRole('button', { name: 'Start Audio' }))
  expect(await screen.findByRole('button', { name: 'Stop Audio' })).toBeTruthy()
  expect(screen.getByText('Audio settings')).toBeTruthy()
})

it('reports failed stop requests without claiming audio stopped', async () => {
  vi.stubGlobal(
    'fetch',
    vi.fn(async (_url: string, init?: RequestInit) =>
      init?.method === 'POST'
        ? { ok: false, status: 503 }
        : { ok: true, status: 200, json: async () => ({ running: true }) },
    ),
  )
  const user = userEvent.setup()
  render(<AudioServiceControl>Audio settings</AudioServiceControl>)
  await user.click(await screen.findByRole('button', { name: 'Stop Audio' }))
  expect((await screen.findByRole('alert')).textContent).toContain(
    'Could not stop audio',
  )
  expect(screen.getByRole('button', { name: 'Stop Audio' })).toBeTruthy()
})

it('does not show the control on unsupported desktop hosts', async () => {
  vi.stubGlobal(
    'fetch',
    vi.fn(async () => ({ ok: false, status: 404 })),
  )
  render(<AudioServiceControl>Audio settings</AudioServiceControl>)
  expect(await screen.findByText('Audio settings')).toBeTruthy()
  await vi.waitFor(() => expect(screen.queryByText('Audio service')).toBeNull())
})
