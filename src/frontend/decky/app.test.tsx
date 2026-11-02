import {
  cleanup,
  fireEvent,
  render,
  screen,
  waitFor,
} from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { forwardRef, type ForwardedRef } from 'react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { DialogButtonProps } from '@decky/ui'

import { DeckyPreviewShell, previewDeckyUi } from '../test-web/preview_ui'
import type { EarTunerSettings, DeckyUiComponents } from './app'
import type { FocusableProps } from '@decky/ui'

const initialSettings: EarTunerSettings = {
  enabled: true,
  separate_channels: false,
  left_mode: 2,
  left_level: 2,
  right_mode: 3,
  right_level: 1,
}

let currentSettings = initialSettings
let settingsLoadFails = false
let settingsLoadStatus = 200
let settingsRequestPending = false
let settingsReadRejects = false
let settingsSaveResult = true
let settingsSaveFails = false
let settingsSavePending = false
let completePendingSettingsSave: ((applied: boolean) => void) | undefined
let audioPlayFails = false
let bypassRequestFails = false
let requests: Array<{ url: string; init?: RequestInit }> = []
let deckySettingsUpdates: EarTunerSettings[] = []
let mockAudio: MockAudio
let completePendingSettingsRead: (() => void) | undefined
let rejectPendingSettingsRead: (() => void) | undefined
let gamepadButtonDown: FocusableProps['onButtonDown'] | undefined
let rerenderQuickAccess: ((visible: boolean) => void) | undefined
let unmountApp: (() => void) | undefined
const storage = new Map<string, string>()
const localStorageMock: Storage = {
  get length() {
    return storage.size
  },
  clear: () => {
    storage.clear()
  },
  getItem: (key: string) => storage.get(key) ?? null,
  key: (index: number) => [...storage.keys()][index] ?? null,
  removeItem: (key: string) => {
    storage.delete(key)
  },
  setItem: (key: string, value: string) => {
    storage.set(key, value)
  },
}

class MockAudio extends EventTarget {
  currentTime = 0
  loop = false
  paused = true
  preload = ''
  volume = 1

  async play(): Promise<void> {
    if (audioPlayFails) throw new Error('Audio playback unavailable')
    this.paused = false
    this.dispatchEvent(new Event('play'))
  }

  pause(): void {
    if (this.paused) return
    this.paused = true
    this.dispatchEvent(new Event('pause'))
  }

  finish(): void {
    this.paused = true
    this.dispatchEvent(new Event('ended'))
  }
}

async function renderApp(
  deckyUi: DeckyUiComponents = previewDeckyUi,
  onOpenInfo?: () => void,
) {
  const { App } = await import('./app')
  const user = userEvent.setup()
  const view = render(
    <DeckyPreviewShell title="Ear Tuner">
      <App deckyUi={deckyUi} onOpenInfo={onOpenInfo} />
    </DeckyPreviewShell>,
  )
  unmountApp = view.unmount
  rerenderQuickAccess = (visible) =>
    view.rerender(
      <DeckyPreviewShell title="Ear Tuner">
        <App
          deckyUi={deckyUi}
          onOpenInfo={onOpenInfo}
          quickAccessVisible={visible}
        />
      </DeckyPreviewShell>,
    )
  await screen.findByText('Current Focus Mode:')
  return user
}

beforeEach(() => {
  currentSettings = { ...initialSettings }
  settingsLoadFails = false
  settingsLoadStatus = 200
  settingsRequestPending = false
  settingsReadRejects = false
  settingsSaveResult = true
  settingsSaveFails = false
  settingsSavePending = false
  completePendingSettingsSave = undefined
  audioPlayFails = false
  bypassRequestFails = false
  requests = []
  deckySettingsUpdates = []
  completePendingSettingsRead = undefined
  rejectPendingSettingsRead = undefined
  gamepadButtonDown = undefined
  rerenderQuickAccess = undefined
  unmountApp = undefined
  mockAudio = new MockAudio()
  storage.clear()
  vi.stubGlobal('localStorage', localStorageMock)
  vi.resetModules()
  const standaloneSettingsApiUrl = '/api/ear-tuner'
  vi.stubGlobal(
    'Audio',
    vi.fn(function MockAudioConstructor() {
      return mockAudio
    }),
  )
  vi.stubGlobal(
    'fetch',
    vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input)
      requests.push({ url, init })

      if (url === standaloneSettingsApiUrl && !init?.method) {
        if (settingsLoadFails) throw new Error('Settings service unavailable')
        if (settingsRequestPending) {
          return new Promise((resolve, reject) => {
            completePendingSettingsRead = () =>
              resolve({
                ok: true,
                json: async () => ({ ...currentSettings }),
              } as Response)
            if (settingsReadRejects) {
              rejectPendingSettingsRead = () =>
                reject(new Error('Settings read failed after unmount'))
            }
          })
        }
        return {
          ok: settingsLoadStatus < 400,
          status: settingsLoadStatus,
          json: async () => ({ ...currentSettings }),
        } as Response
      }
      if (url === standaloneSettingsApiUrl && init?.method === 'POST') {
        if (settingsSaveFails) throw new Error('Settings write failed')
        if (settingsSavePending) {
          return new Promise<Response>((resolve) => {
            completePendingSettingsSave = (applied) =>
              resolve({ ok: applied } as Response)
          })
        }
        return { ok: settingsSaveResult } as Response
      }
      if (url.includes('focus_bypass=') && bypassRequestFails) {
        throw new Error('Audio processing request failed')
      }
      return { ok: true } as Response
    }),
  )
})

afterEach(() => {
  cleanup()
  vi.doUnmock('@decky/api')
  Reflect.deleteProperty(
    window,
    '__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit',
  )
  vi.unstubAllGlobals()
})

describe('Ear Tuner frontend', () => {
  it('shows a loading state until persisted settings arrive', async () => {
    settingsRequestPending = true
    const { App } = await import('./app')
    render(
      <DeckyPreviewShell title="Ear Tuner">
        <App deckyUi={previewDeckyUi} />
      </DeckyPreviewShell>,
    )

    expect(screen.getByRole('status').textContent).toContain(
      'Loading Ear Tuner settings...',
    )
    await waitFor(() => expect(completePendingSettingsRead).toBeTruthy())
    completePendingSettingsRead?.()
    expect(await screen.findByText('Current Focus Mode:')).toBeTruthy()
  })

  it('does not update state if settings resolve after the app unmounts', async () => {
    settingsRequestPending = true
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    const { App } = await import('./app')
    const view = render(
      <DeckyPreviewShell title="Ear Tuner">
        <App deckyUi={previewDeckyUi} />
      </DeckyPreviewShell>,
    )
    await waitFor(() => expect(completePendingSettingsRead).toBeTruthy())

    view.unmount()
    completePendingSettingsRead?.()
    await new Promise((resolve) => window.setTimeout(resolve, 0))

    expect(console.error).not.toHaveBeenCalled()
  })

  it('does not update state if settings reject after the app unmounts', async () => {
    settingsRequestPending = true
    settingsReadRejects = true
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    const { App } = await import('./app')
    const view = render(
      <DeckyPreviewShell title="Ear Tuner">
        <App deckyUi={previewDeckyUi} />
      </DeckyPreviewShell>,
    )
    await waitFor(() => expect(rejectPendingSettingsRead).toBeTruthy())

    view.unmount()
    rejectPendingSettingsRead?.()
    await waitFor(() =>
      expect(console.error).toHaveBeenCalledWith(
        'Could not load Ear Tuner settings:',
        expect.any(Error),
      ),
    )
  })

  it('hydrates persisted settings before presenting controls', async () => {
    currentSettings = { ...initialSettings, enabled: false, left_mode: 4 }

    await renderApp()

    expect(screen.getByText('Disabled')).toBeTruthy()
    expect(screen.getByText('Quiet Sounds')).toBeTruthy()
    expect(screen.queryByText('Loading Ear Tuner settings...')).toBeNull()
  })

  it('uses the intended defaults when settings cannot be loaded', async () => {
    settingsLoadFails = true
    vi.spyOn(console, 'error').mockImplementation(() => undefined)

    await renderApp()

    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('Voices')
  })

  it('uses fallback settings when the standalone settings endpoint is not OK', async () => {
    settingsLoadStatus = 503
    vi.spyOn(console, 'error').mockImplementation(() => undefined)

    await renderApp()

    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('Voices')
    expect(
      requests.some(
        (request) => request.url === '/api/ear-tuner' && !request.init?.method,
      ),
    ).toBe(true)
  })

  it('falls back when stored numeric values are outside their valid ranges', async () => {
    settingsLoadFails = true
    storage.set('focus-mode-left', '8')
    storage.set('focus-level-left', '0')
    vi.spyOn(console, 'error').mockImplementation(() => undefined)

    await renderApp()

    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('Voices')
    expect((screen.getByRole('slider') as HTMLInputElement).value).toBe('2')
  })

  it('preserves an explicitly stored unprocessed mode', async () => {
    settingsLoadFails = true
    storage.set('focus-mode-left', '0')
    vi.spyOn(console, 'error').mockImplementation(() => undefined)

    await renderApp()

    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('None')
  })

  it('falls back when a stored numeric value is empty', async () => {
    settingsLoadFails = true
    storage.set('focus-mode-left', '')
    vi.spyOn(console, 'error').mockImplementation(() => undefined)

    await renderApp()

    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('Voices')
  })

  it('does not overwrite settings when the initial settings read fails', async () => {
    settingsLoadFails = true
    vi.spyOn(console, 'error').mockImplementation(() => undefined)

    const user = await renderApp()
    await new Promise((resolve) => window.setTimeout(resolve, 200))

    const settingsUpdates = () =>
      requests.filter(
        (request) =>
          request.url === '/api/ear-tuner' && request.init?.method === 'POST',
      )
    expect(settingsUpdates()).toHaveLength(0)

    await user.click(screen.getByRole('checkbox', { name: /Enable Ear Tuner/ }))
    await waitFor(() => expect(settingsUpdates()).toHaveLength(1))
    expect(JSON.parse(String(settingsUpdates()[0].init?.body))).toMatchObject({
      enabled: true,
    })
  })

  it('shows independent channel controls only when separate ears is enabled', async () => {
    const user = await renderApp()

    expect(screen.getByRole('heading', { name: 'Both ears' })).toBeTruthy()
    expect(screen.queryByRole('heading', { name: 'Right ear' })).toBeNull()

    await user.click(
      screen.getByRole('checkbox', { name: /Adjust Ears Separately/ }),
    )

    expect(screen.getByRole('heading', { name: 'Left ear' })).toBeTruthy()
    expect(screen.getByRole('heading', { name: 'Right ear' })).toBeTruthy()
  })

  it('changes the right focus mode without changing the left mode', async () => {
    const user = await renderApp()
    await user.click(
      screen.getByRole('checkbox', { name: /Adjust Ears Separately/ }),
    )

    await user.click(screen.getAllByRole('button', { name: 'Change' })[1])
    await user.click(
      await screen.findByRole('button', { name: 'Quiet Sounds' }),
    )

    const selectedModes = document.querySelectorAll(
      '.earTunerSelectedMode strong',
    )
    expect(selectedModes[0].textContent).toBe('Voices')
    expect(selectedModes[1].textContent).toBe('Quiet Sounds')
    await waitFor(() => {
      const updates = requests
        .filter(
          (request) =>
            request.url === '/api/ear-tuner' && request.init?.method === 'POST',
        )
        .map((request) => JSON.parse(String(request.init?.body)))
      expect(updates).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            separate_channels: true,
            left_mode: 2,
            right_mode: 4,
          }),
        ]),
      )
    })
  })

  it('hides adjustment strength for the unprocessed mode', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(await screen.findByRole('button', { name: 'None' }))

    expect(screen.queryByRole('slider')).toBeNull()
  })

  it('persists independent adjustment levels for both channels', async () => {
    const user = await renderApp()
    fireEvent.change(screen.getAllByRole('slider')[0], {
      target: { value: '3' },
    })
    await user.click(
      screen.getByRole('checkbox', { name: /Adjust Ears Separately/ }),
    )
    fireEvent.change(screen.getAllByRole('slider')[1], {
      target: { value: '3' },
    })

    await waitFor(() => {
      const update = requests.find(
        (request) =>
          request.url === '/api/ear-tuner' && request.init?.method === 'POST',
      )
      expect(JSON.parse(String(update?.init?.body))).toMatchObject({
        left_level: 3,
        right_level: 3,
        separate_channels: true,
      })
    })
  })

  it('applies and persists a selected focus mode', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )

    expect(screen.getByText('High Frequencies')).toBeTruthy()
    await waitFor(() => {
      const updates = requests
        .filter(
          (request) =>
            request.url === '/api/ear-tuner' && request.init?.method === 'POST',
        )
        .map((request) => JSON.parse(String(request.init?.body)))
      expect(updates).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            left_mode: 3,
            right_mode: 3,
          }),
        ]),
      )
    })
  })

  it('restores scroll and focus to the mode button', async () => {
    const user = await renderApp()
    const scrollContainer = document.querySelector(
      '.deckyPreviewContent',
    ) as HTMLElement
    Object.defineProperty(scrollContainer, 'scrollHeight', {
      configurable: true,
      value: 600,
    })
    Object.defineProperty(scrollContainer, 'clientHeight', {
      configurable: true,
      value: 300,
    })
    scrollContainer.scrollTop = 72

    for (const overflow of ['auto', 'scroll', 'hidden']) {
      scrollContainer.style.overflowY = overflow
      const changeButton = screen.getByRole('button', { name: 'Change' })
      await user.click(changeButton)
      await user.click(await screen.findByRole('button', { name: 'Return' }))
      await waitFor(() => expect(scrollContainer.scrollTop).toBe(72))
      expect(document.activeElement).toBe(changeButton)
    }
  })

  it('restores mode-menu scroll after the Decky panel remounts', async () => {
    const { App } = await import('./app')
    const user = userEvent.setup()
    const renderPanel = (visible: boolean) => (
      <DeckyPreviewShell title="Ear Tuner">
        {visible ? <App deckyUi={previewDeckyUi} /> : null}
      </DeckyPreviewShell>
    )
    const view = render(renderPanel(true))
    await screen.findByText('Current Focus Mode:')

    const scrollContainer = document.querySelector(
      '.deckyPreviewContent',
    ) as HTMLElement
    Object.defineProperty(scrollContainer, 'scrollHeight', {
      configurable: true,
      value: 600,
    })
    Object.defineProperty(scrollContainer, 'clientHeight', {
      configurable: true,
      value: 300,
    })
    scrollContainer.style.overflowY = 'auto'
    scrollContainer.scrollTop = 72

    await user.click(screen.getByRole('button', { name: 'Change' }))
    view.rerender(renderPanel(false))
    await user.click(await screen.findByRole('button', { name: 'Return' }))

    scrollContainer.scrollTop = 0
    view.rerender(renderPanel(true))
    await screen.findByText('Current Focus Mode:')
    await waitFor(() => expect(scrollContainer.scrollTop).toBe(72))
    expect(document.activeElement).toBe(
      screen.getByRole('button', { name: 'Change' }),
    )
  })

  it('keeps sample playback running while the mode menu is open', async () => {
    const { App } = await import('./app')
    const user = userEvent.setup()
    const renderPanel = (visible: boolean) => (
      <DeckyPreviewShell title="Ear Tuner">
        {visible ? <App deckyUi={previewDeckyUi} /> : null}
      </DeckyPreviewShell>
    )
    const view = render(renderPanel(true))
    await screen.findByText('Current Focus Mode:')

    await user.click(screen.getByRole('button', { name: 'Play' }))
    await user.click(screen.getByRole('button', { name: 'Change' }))
    view.rerender(renderPanel(false))

    expect(mockAudio.paused).toBe(false)
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )
    view.rerender(renderPanel(true))

    expect(mockAudio.paused).toBe(false)
    expect(await screen.findByRole('button', { name: 'Stop' })).toBeTruthy()
  })

  it('ignores temporary Quick Access hiding for the mode menu', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Play' }))
    await user.click(screen.getByRole('button', { name: 'Change' }))
    rerenderQuickAccess?.(false)

    expect(mockAudio.paused).toBe(false)
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )
    rerenderQuickAccess?.(true)

    expect(mockAudio.paused).toBe(false)
    expect(await screen.findByRole('button', { name: 'Stop' })).toBeTruthy()
  })

  it('opens the mode menu when the selector has no focusable button', async () => {
    const deckyUi: DeckyUiComponents = {
      ...previewDeckyUi,
      DialogButton: ({ children, className, onClick }: DialogButtonProps) => (
        <button
          className={className}
          onClick={(event) => {
            if (className === 'earTunerChangeModeButton') {
              event.currentTarget.remove()
            }
            onClick?.(event.nativeEvent)
          }}
        >
          {children}
        </button>
      ),
    }
    const user = await renderApp(deckyUi)

    await user.click(screen.getByRole('button', { name: 'Change' }))

    expect(
      await screen.findByRole('dialog', {
        name: "Select Ear Tuner Area You'd Like To Boost",
      }),
    ).toBeTruthy()
    await user.click(screen.getByRole('button', { name: 'Return' }))
  })

  it('preserves a changed mode across unmount when the next read is stale', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )
    expect(screen.getByText('High Frequencies')).toBeTruthy()
    unmountApp?.()

    await renderApp()

    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('High Frequencies')
  })

  it('restores the previous mode when the backend rejects a mode change', async () => {
    const user = await renderApp()
    settingsSaveResult = false

    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )

    await waitFor(() => {
      expect(
        document.querySelector('.earTunerSelectedMode strong')?.textContent,
      ).toBe('Voices')
    })
  })

  it('restores the previous mode when a mode save request rejects', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    const user = await renderApp()
    settingsSaveFails = true

    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )

    await waitFor(() => {
      expect(
        document.querySelector('.earTunerSelectedMode strong')?.textContent,
      ).toBe('Voices')
    })
  })

  it('uses Decky RPC for settings', async () => {
    const deckyApi = {
      get_ear_tuner: vi.fn(async () => ({ ...currentSettings })),
      set_ear_tuner: vi.fn(async (value: EarTunerSettings) => {
        deckySettingsUpdates.push(value)
        return true
      }),
    }
    vi.doMock('@decky/api', () => ({
      callable: (method: keyof typeof deckyApi) => deckyApi[method],
    }))
    Object.defineProperty(
      window,
      '__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit',
      { configurable: true, value: true },
    )

    const user = await renderApp()
    expect(deckyApi.get_ear_tuner).toHaveBeenCalledOnce()
    await user.click(
      screen.getByRole('checkbox', { name: /Adjust Ears Separately/ }),
    )
    await waitFor(() => expect(deckySettingsUpdates.length).toBeGreaterThan(0))
    expect(deckySettingsUpdates[deckySettingsUpdates.length - 1]).toMatchObject(
      {
        separate_channels: true,
      },
    )

    expect(requests.some((request) => request.url === '/api/ear-tuner')).toBe(
      false,
    )
  })

  it('renders the guide, opens project links, and returns to controls', async () => {
    const { DeckyInfoPage } = await import('./app')
    const onBack = vi.fn()
    const openWindow = vi.spyOn(window, 'open').mockImplementation(() => null)
    const user = userEvent.setup()
    render(
      <DeckyPreviewShell title="Ear Tuner Guide">
        <DeckyInfoPage deckyUi={previewDeckyUi} onBack={onBack} />
      </DeckyPreviewShell>,
    )

    expect(
      screen.getByRole('heading', { name: 'How to Use Ear Tuner' }),
    ).toBeTruthy()
    expect(screen.getByRole('heading', { name: 'Quick Start' })).toBeTruthy()
    expect(screen.getByRole('heading', { name: 'Focus Modes' })).toBeTruthy()
    expect(screen.getByText(/starts a stereo sample/)).toBeTruthy()
    expect(
      screen.getByRole('heading', { name: 'Help & Feedback' }),
    ).toBeTruthy()
    await user.click(screen.getByRole('button', { name: 'Report an Issue' }))
    await user.click(screen.getByRole('button', { name: 'Suggest an Idea' }))
    expect(openWindow).toHaveBeenNthCalledWith(
      1,
      expect.stringContaining('/issues/new'),
      '_blank',
      'noopener,noreferrer',
    )
    expect(openWindow).toHaveBeenNthCalledWith(
      2,
      expect.stringContaining('/discussions/new'),
      '_blank',
      'noopener,noreferrer',
    )

    await user.click(screen.getByRole('button', { name: 'Back to Controls' }))
    expect(onBack).toHaveBeenCalledOnce()
  })

  it('restores scroll and focus to the Guide button after returning', async () => {
    const { App } = await import('./app')
    const onOpenInfo = vi.fn()
    const user = userEvent.setup()
    const renderPanel = (visible: boolean) => (
      <DeckyPreviewShell title="Ear Tuner">
        {visible ? (
          <App deckyUi={previewDeckyUi} onOpenInfo={onOpenInfo} />
        ) : null}
      </DeckyPreviewShell>
    )
    const view = render(renderPanel(true))
    await screen.findByText('Current Focus Mode:')
    const scrollContainer = document.querySelector(
      '.deckyPreviewContent',
    ) as HTMLElement
    Object.defineProperty(scrollContainer, 'scrollHeight', {
      configurable: true,
      value: 600,
    })
    Object.defineProperty(scrollContainer, 'clientHeight', {
      configurable: true,
      value: 300,
    })
    scrollContainer.style.overflowY = 'auto'
    scrollContainer.scrollTop = 144

    await user.click(screen.getByRole('button', { name: 'Ear Tuner Guide' }))
    expect(onOpenInfo).toHaveBeenCalledOnce()
    view.rerender(renderPanel(false))
    scrollContainer.scrollTop = 0
    view.rerender(renderPanel(true))

    await screen.findByText('Current Focus Mode:')
    await waitFor(() => expect(scrollContainer.scrollTop).toBe(144))
    expect(document.activeElement).toBe(
      screen.getByRole('button', { name: 'Ear Tuner Guide' }),
    )
  })

  it('returns from the Guide without leaving the plugin', async () => {
    const { DeckyPluginContent } = await import('./app')
    const user = userEvent.setup()
    render(
      <DeckyPreviewShell title="Ear Tuner">
        <DeckyPluginContent deckyUi={previewDeckyUi} />
      </DeckyPreviewShell>,
    )
    await screen.findByText('Current Focus Mode:')

    await user.click(screen.getByRole('button', { name: 'Ear Tuner Guide' }))
    expect(
      screen.getByRole('heading', { name: 'How to Use Ear Tuner' }),
    ).toBeTruthy()

    await user.click(screen.getByRole('button', { name: 'Back to Controls' }))

    await screen.findByText('Current Focus Mode:')
    await waitFor(() =>
      expect(document.activeElement).toBe(
        screen.getByRole('button', { name: 'Ear Tuner Guide' }),
      ),
    )
  })

  it('returns to controls when the guide receives controller cancel', async () => {
    const { DeckyInfoPage } = await import('./app')
    const onBack = vi.fn()
    let onCancel: FocusableProps['onCancel']
    const deckyUi: DeckyUiComponents = {
      ...previewDeckyUi,
      Focusable: ({ children, className, onCancel: handleCancel }) => {
        if (className?.includes('deckyInfoPage')) onCancel = handleCancel
        return <div className={className}>{children}</div>
      },
    }
    render(
      <DeckyPreviewShell title="Ear Tuner Guide">
        <DeckyInfoPage deckyUi={deckyUi} onBack={onBack} />
      </DeckyPreviewShell>,
    )

    const stopPropagation = vi.fn()
    onCancel?.({ stopPropagation } as unknown as Parameters<
      NonNullable<FocusableProps['onCancel']>
    >[0])

    expect(stopPropagation).toHaveBeenCalledOnce()
    expect(onBack).toHaveBeenCalledOnce()
  })

  it('opens at the top and returns there when controller focus moves up', async () => {
    const { DeckyInfoPage } = await import('./app')
    let activateGuideContent: FocusableProps['onActivate']
    let focusGuideContent: FocusableProps['onGamepadFocus']
    let guideEntryPosition: FocusableProps['navEntryPreferPosition']
    let finishInitialFocus: FrameRequestCallback | undefined
    vi.spyOn(window, 'requestAnimationFrame').mockImplementation((callback) => {
      finishInitialFocus = callback
      return 1
    })
    const deckyUi: DeckyUiComponents = {
      ...previewDeckyUi,
      Focusable: forwardRef(function TestFocusable(
        {
          children,
          className,
          navEntryPreferPosition,
          onActivate,
          onGamepadFocus,
          tabIndex,
        },
        ref: ForwardedRef<HTMLDivElement>,
      ) {
        if (className?.includes('deckyInfoPage')) {
          guideEntryPosition = navEntryPreferPosition
        }
        if (className === 'deckyGuideTop') {
          activateGuideContent = onActivate
          focusGuideContent = onGamepadFocus
        }
        return (
          <div ref={ref} className={className} tabIndex={tabIndex}>
            {children}
          </div>
        )
      }),
    }
    render(
      <DeckyPreviewShell title="Ear Tuner Guide">
        <DeckyInfoPage deckyUi={deckyUi} onBack={vi.fn()} />
      </DeckyPreviewShell>,
    )
    const guideContent = document.querySelector('.deckyGuideTop') as HTMLElement
    expect(guideEntryPosition).toBe(0)
    expect(guideContent.tabIndex).toBe(0)
    expect(guideContent.querySelector('h3')).toBeNull()
    expect(activateGuideContent).toBeTypeOf('function')
    const scrollContainer = document.querySelector(
      '.deckyPreviewContent',
    ) as HTMLElement
    const outerScrollContainer = scrollContainer.parentElement as HTMLElement
    Object.defineProperty(scrollContainer, 'scrollHeight', {
      configurable: true,
      value: 600,
    })
    Object.defineProperty(scrollContainer, 'clientHeight', {
      configurable: true,
      value: 300,
    })
    scrollContainer.style.overflowY = 'auto'
    scrollContainer.scrollTop = 300
    Object.defineProperty(outerScrollContainer, 'scrollHeight', {
      configurable: true,
      value: 700,
    })
    Object.defineProperty(outerScrollContainer, 'clientHeight', {
      configurable: true,
      value: 300,
    })
    outerScrollContainer.style.overflowY = 'auto'
    outerScrollContainer.scrollTop = 180

    focusGuideContent?.({
      currentTarget: guideContent,
    } as unknown as Parameters<
      NonNullable<FocusableProps['onGamepadFocus']>
    >[0])

    expect(scrollContainer.scrollTop).toBe(0)
    expect(outerScrollContainer.scrollTop).toBe(0)

    scrollContainer.scrollTop = 12
    outerScrollContainer.scrollTop = 12
    finishInitialFocus?.(0)
    expect(scrollContainer.scrollTop).toBe(0)
    expect(outerScrollContainer.scrollTop).toBe(0)

    scrollContainer.scrollTop = 240
    outerScrollContainer.scrollTop = 160
    focusGuideContent?.({
      currentTarget: guideContent,
    } as unknown as Parameters<
      NonNullable<FocusableProps['onGamepadFocus']>
    >[0])

    expect(scrollContainer.scrollTop).toBe(0)
    expect(outerScrollContainer.scrollTop).toBe(0)
  })

  it('restores processing on load without exposing a comparison toggle', async () => {
    await renderApp()

    await waitFor(() => {
      expect(
        requests.some((request) => request.url.includes('focus_bypass=0')),
      ).toBe(true)
    })
    expect(
      screen.queryByRole('checkbox', { name: /Compare Ear Tuner Settings/ }),
    ).toBeNull()
    expect(
      requests.some((request) => request.url.includes('focus_bypass=1')),
    ).toBe(false)
  })

  it('reverts Enable when the backend rejects the settings update', async () => {
    currentSettings = { ...initialSettings, enabled: false }
    settingsSaveResult = false
    const user = await renderApp()

    await user.click(screen.getByRole('checkbox', { name: /Enable Ear Tuner/ }))

    await waitFor(() => expect(screen.getByText('Disabled')).toBeTruthy())
    expect(
      (
        screen.getByRole('checkbox', {
          name: /Enable Ear Tuner/,
        }) as HTMLInputElement
      ).checked,
    ).toBe(false)
  })

  it('uses native control fallback when Decky RPC cannot load', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    storage.set('focus-enabled', 'true')
    storage.set('focus-mode-right', '3')
    storage.set('focus-level-right', '1')
    vi.doMock('@decky/api', () => {
      throw new Error('Decky RPC unavailable')
    })
    Object.defineProperty(
      window,
      '__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit',
      { configurable: true, value: true },
    )

    const user = await renderApp()
    await waitFor(() => {
      expect(
        requests.some((request) =>
          request.url.includes('/api/v1/control?focus_enabled=1'),
        ),
      ).toBe(true)
    })
    await user.click(
      screen.getByRole('checkbox', { name: /Adjust Ears Separately/ }),
    )
    await waitFor(() =>
      expect(
        requests.some((request) =>
          request.url.includes('right_mode=3&right_level=1'),
        ),
      ).toBe(true),
    )
    const nativeRequestCount = requests.filter((request) =>
      request.url.startsWith('http://127.0.0.1:19285/api/v1/control?'),
    ).length

    await user.click(screen.getAllByRole('button', { name: 'Change' })[0])
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )

    await waitFor(() => {
      expect(
        requests.filter((request) =>
          request.url.startsWith('http://127.0.0.1:19285/api/v1/control?'),
        ).length,
      ).toBeGreaterThan(nativeRequestCount)
    })
    const modeRequests = requests.filter((request) =>
      request.url.startsWith('http://127.0.0.1:19285/api/v1/control?'),
    )
    const modeRequest = modeRequests[modeRequests.length - 1]
    expect(
      new URL(String(modeRequest?.url)).searchParams.get('left_mode'),
    ).toBe('3')
    expect(
      new URL(String(modeRequest?.url)).searchParams.get('right_mode'),
    ).toBe('3')
  })

  it('uses the native settings fallback while Ear Tuner is disabled', async () => {
    storage.set('focus-enabled', 'false')
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    vi.doMock('@decky/api', () => {
      throw new Error('Decky RPC unavailable')
    })
    Object.defineProperty(
      window,
      '__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit',
      { configurable: true, value: true },
    )

    await renderApp()

    await waitFor(() =>
      expect(
        requests.some((request) =>
          request.url.includes('/api/v1/control?focus_enabled=0'),
        ),
      ).toBe(true),
    )
  })

  it('sends shared mode changes through the disabled native fallback', async () => {
    storage.set('focus-enabled', 'false')
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    vi.doMock('@decky/api', () => {
      throw new Error('Decky RPC unavailable')
    })
    Object.defineProperty(
      window,
      '__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit',
      { configurable: true, value: true },
    )
    const deckyUi: DeckyUiComponents = {
      ...previewDeckyUi,
      DialogButton: ({ children, className, onClick }: DialogButtonProps) => (
        <button
          className={className}
          onClick={(event) => onClick?.(event.nativeEvent)}
        >
          {children}
        </button>
      ),
    }
    const user = await renderApp(deckyUi)

    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )

    await waitFor(() =>
      expect(
        requests.some((request) =>
          request.url.includes(
            'focus_enabled=0&left_mode=3&left_level=2&right_mode=3&right_level=2',
          ),
        ),
      ).toBe(true),
    )
  })

  it('logs a failed processing restore without crashing the controls', async () => {
    bypassRequestFails = true
    const logError = vi
      .spyOn(console, 'error')
      .mockImplementation(() => undefined)

    await renderApp()

    await waitFor(() =>
      expect(logError).toHaveBeenCalledWith(
        'Could not restore audio processing:',
        expect.any(Error),
      ),
    )
    expect(screen.getByRole('button', { name: 'Change' })).toBeTruthy()
  })

  it('restores the right mode when the backend rejects a right-channel change', async () => {
    const user = await renderApp()
    await user.click(
      screen.getByRole('checkbox', { name: /Adjust Ears Separately/ }),
    )
    await waitFor(() =>
      expect(
        requests.some(
          (request) =>
            request.init?.method === 'POST' &&
            JSON.parse(String(request.init.body)).separate_channels === true,
        ),
      ).toBe(true),
    )
    settingsSaveResult = false

    await user.click(screen.getAllByRole('button', { name: 'Change' })[1])
    await user.click(
      await screen.findByRole('button', { name: 'Quiet Sounds' }),
    )

    await waitFor(() => {
      const modes = document.querySelectorAll('.earTunerSelectedMode strong')
      expect(modes[0].textContent).toBe('Voices')
      expect(modes[1].textContent).toBe('High Frequencies')
    })
  })

  it('reverts Enable when the settings request rejects', async () => {
    currentSettings = { ...initialSettings, enabled: false }
    settingsSaveFails = true
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    const user = await renderApp()

    await user.click(screen.getByRole('checkbox', { name: /Enable Ear Tuner/ }))

    await waitFor(() => expect(screen.getByText('Disabled')).toBeTruthy())
    expect(
      (
        screen.getByRole('checkbox', {
          name: /Enable Ear Tuner/,
        }) as HTMLInputElement
      ).checked,
    ).toBe(false)
  })

  it('restores the acknowledged enabled state and reports a failed Disable', async () => {
    const logError = vi
      .spyOn(console, 'error')
      .mockImplementation(() => undefined)
    const user = await renderApp()
    await waitFor(() =>
      expect(
        requests.some(
          (request) =>
            request.url === '/api/ear-tuner' && request.init?.method === 'POST',
        ),
      ).toBe(true),
    )
    settingsSaveFails = true

    await user.click(screen.getByRole('checkbox', { name: /Enable Ear Tuner/ }))

    await waitFor(() =>
      expect(logError).toHaveBeenCalledWith(
        'Could not save Ear Tuner settings:',
        expect.any(Error),
      ),
    )
    await waitFor(() => expect(screen.getByText('Enabled')).toBeTruthy())
    expect(screen.getByRole('alert').textContent).toContain(
      'Settings were not confirmed',
    )
    expect(storage.get('focus-enabled')).toBe('true')
  })

  it('restores Enabled when the backend rejects Disable without throwing', async () => {
    const user = await renderApp()
    await waitFor(() =>
      expect(requests.some((request) => request.init?.method === 'POST')).toBe(
        true,
      ),
    )
    settingsSaveResult = false
    await user.click(screen.getByRole('checkbox', { name: /Enable Ear Tuner/ }))
    await waitFor(() => expect(screen.getByRole('alert')).toBeTruthy())
    expect(screen.getByText('Enabled')).toBeTruthy()
  })

  it('serializes settings writes and ignores failures superseded by a newer change', async () => {
    const user = await renderApp()
    await waitFor(() =>
      expect(requests.some((request) => request.init?.method === 'POST')).toBe(
        true,
      ),
    )
    const writesBefore = requests.filter(
      (request) => request.init?.method === 'POST',
    ).length
    settingsSavePending = true
    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'Low Frequencies' }),
    )
    await waitFor(() => expect(completePendingSettingsSave).toBeDefined())
    await user.click(screen.getByRole('button', { name: 'Change' }))
    await user.click(
      await screen.findByRole('button', { name: 'High Frequencies' }),
    )
    expect(
      requests.filter((request) => request.init?.method === 'POST'),
    ).toHaveLength(writesBefore + 1)
    settingsSavePending = false
    completePendingSettingsSave?.(false)
    await waitFor(() =>
      expect(
        requests.filter((request) => request.init?.method === 'POST'),
      ).toHaveLength(writesBefore + 2),
    )
    expect(
      document.querySelector('.earTunerSelectedMode strong')?.textContent,
    ).toBe('High Frequencies')
    expect(screen.queryByRole('alert')).toBeNull()
  })

  it('reports sample playback errors and leaves Play available to retry', async () => {
    audioPlayFails = true
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Play' }))

    expect((await screen.findByRole('alert')).textContent).toContain(
      'Sample unavailable. Try again.',
    )
    expect(screen.getByRole('button', { name: 'Play' })).toBeTruthy()
  })

  it('places listening guidance directly below the headphone notice', async () => {
    await renderApp()

    const notice = screen.getByRole('note')
    const enableControl = screen.getByRole('checkbox', {
      name: /Enable Ear Tuner/,
    })
    const productSummary = screen.getByText(
      'Make voices, bass, higher pitches, or quiet sounds easier to hear.',
    )
    const sampleInstructions = screen.getByText(
      'Play audio from a game, or use the sample below while tuning.',
    ).parentElement
    const adjustmentInstructions = screen.getByText(
      'Choose whether to adjust both ears together or each ear separately. Then select a focus mode and strength.',
    ).parentElement
    const sampleSection = screen
      .getByRole('heading', { name: 'Listening Sample' })
      .closest('section')

    expect(sampleSection).toBeTruthy()
    expect(notice.compareDocumentPosition(enableControl)).toBe(
      Node.DOCUMENT_POSITION_FOLLOWING,
    )
    expect(productSummary.compareDocumentPosition(enableControl)).toBe(
      Node.DOCUMENT_POSITION_FOLLOWING,
    )
    expect(notice.compareDocumentPosition(sampleSection as Node)).toBe(
      Node.DOCUMENT_POSITION_FOLLOWING,
    )
    expect(
      sampleInstructions?.classList.contains('earTunerUsageInstructions'),
    ).toBe(true)
    expect(
      adjustmentInstructions?.classList.contains('earTunerUsageInstructions'),
    ).toBe(true)
    expect(
      (sampleInstructions as HTMLElement).compareDocumentPosition(
        sampleSection as Node,
      ),
    ).toBe(Node.DOCUMENT_POSITION_FOLLOWING)
    expect(
      (sampleSection as Node).compareDocumentPosition(
        adjustmentInstructions as HTMLElement,
      ),
    ).toBe(Node.DOCUMENT_POSITION_FOLLOWING)
  })

  it('stops playback from the button and returns to Play on the ended event', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Play' }))
    mockAudio.currentTime = 15
    await user.click(screen.getByRole('button', { name: 'Stop' }))

    expect(mockAudio.paused).toBe(true)
    expect(mockAudio.currentTime).toBe(0)
    await user.click(screen.getByRole('button', { name: 'Play' }))
    mockAudio.finish()

    await waitFor(() =>
      expect(screen.getByRole('button', { name: 'Play' })).toBeTruthy(),
    )
  })

  it('loads only sample metadata at full source volume before playback', async () => {
    await renderApp()

    await waitFor(() => expect(mockAudio.preload).toBe('metadata'))
    expect(mockAudio.volume).toBe(1)
  })

  it('shows media error events and clears the error when playback retries', async () => {
    const user = await renderApp()
    await screen.findByRole('button', { name: 'Play' })
    mockAudio.dispatchEvent(new Event('error'))
    expect(await screen.findByRole('alert')).toBeTruthy()

    await user.click(screen.getByRole('button', { name: 'Play' }))

    expect(screen.queryByRole('alert')).toBeNull()
    expect(screen.getByRole('button', { name: 'Stop' })).toBeTruthy()
  })

  it('stops sample playback when Quick Access is hidden', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Play' }))
    expect(mockAudio.paused).toBe(false)
    rerenderQuickAccess?.(false)

    await waitFor(() => expect(mockAudio.paused).toBe(true))
    expect(mockAudio.currentTime).toBe(0)
    expect(screen.getByRole('button', { name: 'Play' })).toBeTruthy()
  })

  it('stops sample playback when the Decky panel unmounts', async () => {
    const { App } = await import('./app')
    const user = userEvent.setup()
    const view = render(
      <DeckyPreviewShell title="Ear Tuner">
        <App deckyUi={previewDeckyUi} />
      </DeckyPreviewShell>,
    )
    await screen.findByText('Current Focus Mode:')

    await user.click(screen.getByRole('button', { name: 'Play' }))
    expect(mockAudio.paused).toBe(false)
    view.unmount()

    expect(mockAudio.paused).toBe(true)
    expect(mockAudio.currentTime).toBe(0)
  })

  it('keeps sample playback running when the document becomes hidden', async () => {
    const visibility = vi
      .spyOn(document, 'visibilityState', 'get')
      .mockReturnValue('visible')
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Play' }))
    document.dispatchEvent(new Event('visibilitychange'))
    expect(mockAudio.paused).toBe(false)

    visibility.mockReturnValue('hidden')
    document.dispatchEvent(new Event('visibilitychange'))

    expect(mockAudio.paused).toBe(false)
  })

  it('opens the guide from the right bumper and stops propagation', async () => {
    const onOpenInfo = vi.fn()
    const deckyUi: DeckyUiComponents = {
      ...previewDeckyUi,
      Focusable: ({ children, className, onButtonDown }) => {
        gamepadButtonDown = onButtonDown
        return <div className={className}>{children}</div>
      },
    }
    await renderApp(deckyUi, onOpenInfo)
    const stopPropagation = vi.fn()
    const buttonDown = gamepadButtonDown

    buttonDown?.({
      detail: { button: previewDeckyUi.GamepadButton.BUMPER_RIGHT },
      stopPropagation,
    } as unknown as Parameters<NonNullable<typeof buttonDown>>[0])
    expect(stopPropagation).toHaveBeenCalledOnce()
    expect(onOpenInfo).toHaveBeenCalledOnce()

    buttonDown?.({
      detail: { button: previewDeckyUi.GamepadButton.BUMPER_LEFT },
      stopPropagation,
    } as unknown as Parameters<NonNullable<typeof buttonDown>>[0])
    expect(stopPropagation).toHaveBeenCalledOnce()
    expect(onOpenInfo).toHaveBeenCalledOnce()
  })

  it('keeps playback and Stop available when Ear Tuner is disabled', async () => {
    const user = await renderApp()

    await user.click(screen.getByRole('button', { name: 'Play' }))
    expect(await screen.findByRole('button', { name: 'Stop' })).toBeTruthy()

    await user.click(screen.getByRole('checkbox', { name: /Enable Ear Tuner/ }))

    expect(mockAudio.paused).toBe(false)
    await user.click(screen.getByRole('button', { name: 'Stop' }))

    expect(mockAudio.paused).toBe(true)
    expect(screen.getByRole('button', { name: 'Play' })).toBeTruthy()
    expect(
      (screen.getByRole('button', { name: 'Play' }) as HTMLButtonElement)
        .disabled,
    ).toBe(true)
  })
})
