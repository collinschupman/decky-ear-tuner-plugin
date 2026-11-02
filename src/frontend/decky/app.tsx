import React from 'react'
import type {
  ButtonItemProps,
  DialogButtonProps,
  FocusableProps,
  PanelSectionProps,
  PanelSectionRowProps,
  SliderFieldProps,
  ToggleFieldProps,
} from '@decky/ui'
import { auditionSampleUrl } from './audition_sample'
import './styles.css'

const NATIVE_CONTROL_API_URL = 'http://127.0.0.1:19285/api/v1/control'
const AUDITION_SAMPLE_VOLUME = 1
const GITHUB_REPOSITORY_URL =
  'https://github.com/collinschupman/decky-ear-tuner-plugin'
const BUG_REPORT_URL = `${GITHUB_REPOSITORY_URL}/issues/new?template=bug_report.yml`
const FEATURE_REQUEST_URL = `${GITHUB_REPOSITORY_URL}/discussions/new?category=ideas`
const PRODUCT_SUMMARY =
  'Make voices, bass, higher pitches, or quiet sounds easier to hear.'
const HEADPHONE_ADVISORY =
  'For stereo headphones. Other outputs may not sound as intended.'
const SEPARATE_EARS_LABEL = 'Adjust Ears Separately'
const PLAY_SAMPLE_LABEL = 'Play'
const STOP_SAMPLE_LABEL = 'Stop'
const STANDALONE_SETTINGS_API_URL = '/api/ear-tuner'

const FOCUS_MODE_MENU_OPTIONS = [
  { value: 1, label: 'Low Frequencies' },
  { value: 2, label: 'Voices' },
  { value: 3, label: 'High Frequencies' },
  { value: 4, label: 'Quiet Sounds' },
  { value: 0, label: 'None' },
] as const

type FocusMode = 0 | 1 | 2 | 3 | 4
type FocusLevel = 1 | 2 | 3
type Channel = 'left' | 'right'
type EarTunerSettings = {
  enabled: boolean
  separate_channels: boolean
  left_mode: FocusMode
  left_level: FocusLevel
  right_mode: FocusMode
  right_level: FocusLevel
}

type DeckyPersistence = {
  get: () => Promise<EarTunerSettings>
  set: (value: EarTunerSettings) => Promise<boolean>
}

type DeckyContextMenu = {
  Hide: () => void
}

type DeckyUiComponents = {
  GamepadButton: {
    BUMPER_LEFT: number
    BUMPER_RIGHT: number
  }
  PanelSection: React.ComponentType<PanelSectionProps>
  PanelSectionRow: React.ComponentType<PanelSectionRowProps>
  Focusable: React.ComponentType<
    FocusableProps & React.RefAttributes<HTMLDivElement>
  >
  DialogButton: React.ComponentType<DialogButtonProps>
  ToggleField: React.ComponentType<ToggleFieldProps>
  Menu: React.ComponentType<{
    label: string
    cancelText?: string
    onCancel?: () => void
    children?: React.ReactNode
  }>
  MenuItem: React.ComponentType<{
    children?: React.ReactNode
    selected?: boolean
    onClick?: (event: Event) => void
  }>
  SliderField: React.ComponentType<SliderFieldProps>
  ButtonItem: React.ComponentType<ButtonItemProps>
  showContextMenu: (
    children: React.ReactNode,
    parent?: EventTarget,
  ) => DeckyContextMenu
}

let deckyPersistencePromise: Promise<DeckyPersistence | null> | undefined
let latestEarTunerSettings: EarTunerSettings | null = null
let confirmedEarTunerSettings: EarTunerSettings | null = null
let settingsWriteQueue = Promise.resolve()
let pendingModeMenuRestore:
  { channel: Channel; scrollTop: number | undefined } | undefined
let pendingGuideRestore: { scrollTop: number | undefined } | undefined
let auditionAudio: HTMLAudioElement | undefined
let modeMenuOpen = false

function getAuditionAudio(): HTMLAudioElement {
  if (!auditionAudio) {
    auditionAudio = new Audio(auditionSampleUrl)
    auditionAudio.preload = 'metadata'
    auditionAudio.loop = true
    auditionAudio.volume = AUDITION_SAMPLE_VOLUME
  }
  return auditionAudio
}

export function stopAuditionSample(): void {
  if (!auditionAudio) return
  auditionAudio.pause()
  auditionAudio.currentTime = 0
}

function getStandalonePersistence(): DeckyPersistence {
  return {
    get: () =>
      fetch(STANDALONE_SETTINGS_API_URL).then((response) => {
        if (!response.ok)
          throw new Error(`Desktop settings request failed: ${response.status}`)
        return response.json() as Promise<EarTunerSettings>
      }),
    set: (value) =>
      fetch(STANDALONE_SETTINGS_API_URL, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(value),
      }).then((response) => response.ok),
  }
}

function getDeckyPersistence(): Promise<DeckyPersistence | null> {
  if (!deckyPersistencePromise) {
    deckyPersistencePromise =
      window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit
        ? import('@decky/api')
            .then(({ callable }) => ({
              get: callable<[], EarTunerSettings>('get_ear_tuner'),
              set: callable<[EarTunerSettings], boolean>('set_ear_tuner'),
            }))
            .catch((error) => {
              console.error('Could not connect to Decky settings:', error)
              return null
            })
        : Promise.resolve(getStandalonePersistence())
  }
  return deckyPersistencePromise
}

function saveEarTunerSettings(value: EarTunerSettings): Promise<void> {
  const result = settingsWriteQueue.then(async () => {
    const persistence = await getDeckyPersistence()
    let applied: boolean
    if (persistence) {
      applied = await persistence.set(value)
    } else {
      const params = new URLSearchParams({
        focus_enabled: value.enabled ? '1' : '0',
        left_mode: String(value.left_mode),
        left_level: String(value.left_level),
        right_mode: String(
          value.separate_channels ? value.right_mode : value.left_mode,
        ),
        right_level: String(
          value.separate_channels ? value.right_level : value.left_level,
        ),
      })
      applied = (await fetch(`${NATIVE_CONTROL_API_URL}?${params}`)).ok
    }
    if (!applied)
      throw new Error('The backend did not confirm the settings update')
    confirmedEarTunerSettings = value
  })
  settingsWriteQueue = result.then(
    () => undefined,
    () => undefined,
  )
  return result
}

function sameSettings(
  left: EarTunerSettings,
  right: EarTunerSettings,
): boolean {
  return (
    left.enabled === right.enabled &&
    left.separate_channels === right.separate_channels &&
    left.left_mode === right.left_mode &&
    left.left_level === right.left_level &&
    left.right_mode === right.right_mode &&
    left.right_level === right.right_level
  )
}

function cacheSettings(value: EarTunerSettings): void {
  localStorage.setItem('focus-enabled', String(value.enabled))
  localStorage.setItem(
    'focus-separate-channels',
    String(value.separate_channels),
  )
  localStorage.setItem('focus-mode-left', String(value.left_mode))
  localStorage.setItem('focus-mode-right', String(value.right_mode))
  localStorage.setItem('focus-level-left', String(value.left_level))
  localStorage.setItem('focus-level-right', String(value.right_level))
}

function openFeedbackUrl(url: string): void {
  window.open(url, '_blank', 'noopener,noreferrer')
}

function findScrollableAncestor(element: HTMLElement): HTMLElement | null {
  for (
    let ancestor = element.parentElement;
    ancestor;
    ancestor = ancestor.parentElement
  ) {
    const overflowY = window.getComputedStyle(ancestor).overflowY
    if (
      overflowY !== 'visible' &&
      overflowY !== 'clip' &&
      ancestor.scrollHeight > ancestor.clientHeight
    ) {
      return ancestor
    }
  }
  return null
}

function resetScrollableAncestors(element: HTMLElement): void {
  for (
    let ancestor = element.parentElement;
    ancestor;
    ancestor = ancestor.parentElement
  ) {
    const overflowY = window.getComputedStyle(ancestor).overflowY
    if (
      overflowY !== 'visible' &&
      overflowY !== 'clip' &&
      ancestor.scrollHeight > ancestor.clientHeight
    ) {
      ancestor.scrollTop = 0
    }
  }
}

function GitHubIcon() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <path
        fill="currentColor"
        d="M12 .7a11.5 11.5 0 0 0-3.6 22.4c.6.1.8-.2.8-.6v-2.2c-3.3.7-4-1.4-4-1.4-.5-1.4-1.3-1.7-1.3-1.7-1.1-.8.1-.8.1-.8 1.2.1 1.8 1.2 1.8 1.2 1.1 1.8 2.8 1.3 3.5 1 .1-.8.4-1.3.8-1.6-2.7-.3-5.5-1.3-5.5-5.7 0-1.3.5-2.3 1.2-3.1-.1-.3-.5-1.6.1-3.1 0 0 1-.3 3.2 1.2a11.1 11.1 0 0 1 5.8 0C17 4.8 18 5.1 18 5.1c.6 1.5.2 2.8.1 3.1.8.8 1.2 1.8 1.2 3.1 0 4.4-2.8 5.4-5.5 5.7.4.4.8 1.1.8 2.1v3.4c0 .4.2.7.8.6A11.5 11.5 0 0 0 12 .7Z"
      />
    </svg>
  )
}

function QrCodeIcon() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <path
        fill="currentColor"
        d="M3 3h7v7H3V3Zm2 2v3h3V5H5Zm9-2h7v7h-7V3Zm2 2v3h3V5h-3ZM3 14h7v7H3v-7Zm2 2v3h3v-3H5Zm9-2h3v3h-3v-3Zm4 0h3v7h-3v-3h-2v3h-2v-3h2v-2h2v-2Z"
      />
    </svg>
  )
}

function DeckyGitHubLink({
  deckyUi,
  label,
  url,
}: {
  deckyUi: DeckyUiComponents
  label: string
  url: string
}) {
  const { DialogButton } = deckyUi
  const openUrl = () => openFeedbackUrl(url)

  return (
    <div className="deckyGitHubLink">
      <span className="deckyGitHubIcon">
        <GitHubIcon />
      </span>
      <DialogButton className="deckyGitHubButton" onClick={openUrl}>
        <span className="deckyGitHubButtonContent">
          <span>{label}</span>
          <QrCodeIcon />
        </span>
      </DialogButton>
    </div>
  )
}

export function DeckyInfoPage({
  deckyUi,
  onBack,
}: {
  deckyUi: DeckyUiComponents
  onBack: () => void
}) {
  const { ButtonItem, Focusable } = deckyUi
  const guidePageRef = React.useRef<HTMLDivElement>(null)

  const resetGuideScroll = React.useCallback((element: HTMLElement) => {
    resetScrollableAncestors(element)
    window.requestAnimationFrame(() => {
      resetScrollableAncestors(element)
    })
  }, [])

  React.useLayoutEffect(() => {
    if (guidePageRef.current) resetGuideScroll(guidePageRef.current)
  }, [resetGuideScroll])

  const returnToControls = (event: Event) => {
    event.stopPropagation()
    onBack()
  }

  const scrollGuideToTop = (event: Event) => {
    const guideTop = event.currentTarget
    if (guideTop instanceof HTMLElement) resetGuideScroll(guideTop)
  }

  return (
    <Focusable
      ref={guidePageRef}
      className="earTunerPlugin deckyPanel deckyInfoPage"
      flow-children="vertical"
      navEntryPreferPosition={0}
      onCancel={returnToControls}
      onCancelActionDescription="Back to Controls"
      onCancelButton={returnToControls}
    >
      <Focusable
        aria-label="Beginning of Ear Tuner guide"
        className="deckyGuideTop"
        noFocusRing
        preferredFocus
        onActivate={scrollGuideToTop}
        onGamepadFocus={scrollGuideToTop}
        tabIndex={0}
      >
        <h2>How to Use Ear Tuner</h2>
        <p>
          Tune both ears together, or adjust each ear independently when their
          needs differ.
        </p>
      </Focusable>

      <h3>Quick Start</h3>
      <ol>
        <li>
          Turn on <strong>Enable Ear Tuner</strong>.
        </li>
        <li>
          Play audio from a game, or start the <strong>Listening Sample</strong>
          .
        </li>
        <li>
          Turn on <strong>{SEPARATE_EARS_LABEL}</strong> only when each ear
          needs different settings.
        </li>
        <li>
          Select <strong>Change</strong>, choose a focus mode, then set its
          strength from Weak to Strong.
        </li>
      </ol>

      <h3>Focus Modes</h3>
      <ul>
        <li>
          <strong>Low Frequencies</strong> emphasizes bass and lower-pitched
          sounds.
        </li>
        <li>
          <strong>Voices</strong> brings speech and dialogue forward.
        </li>
        <li>
          <strong>High Frequencies</strong> emphasizes higher-pitched sounds.
        </li>
        <li>
          <strong>Quiet Sounds</strong> raises softer sounds while controlling
          louder peaks.
        </li>
        <li>
          <strong>None</strong> leaves the selected ear unprocessed.
        </li>
      </ul>

      <h3>Listening Sample</h3>
      <p>
        <strong>{PLAY_SAMPLE_LABEL}</strong> starts a stereo sample that repeats
        while you adjust settings. It continues through menus until you select{' '}
        <strong>{STOP_SAMPLE_LABEL}</strong>.
      </p>

      <h3>Help & Feedback</h3>
      <DeckyGitHubLink
        deckyUi={deckyUi}
        label="Report an Issue"
        url={BUG_REPORT_URL}
      />
      <DeckyGitHubLink
        deckyUi={deckyUi}
        label="Suggest an Idea"
        url={FEATURE_REQUEST_URL}
      />
      <ButtonItem
        tooltip="Return to the Ear Tuner plugin."
        layout="below"
        onClick={onBack}
      >
        Back to Controls
      </ButtonItem>
      <div className="versionLabel">Version {__APP_VERSION__}</div>
    </Focusable>
  )
}

export function DeckyPluginContent({
  deckyUi,
  quickAccessVisible = true,
}: {
  deckyUi: DeckyUiComponents
  quickAccessVisible?: boolean
}) {
  const [showGuide, setShowGuide] = React.useState(false)

  return showGuide ? (
    <DeckyInfoPage deckyUi={deckyUi} onBack={() => setShowGuide(false)} />
  ) : (
    <App
      deckyUi={deckyUi}
      onOpenInfo={() => setShowGuide(true)}
      quickAccessVisible={quickAccessVisible}
    />
  )
}

function readStoredNumber<T extends number>(
  key: string,
  fallback: T,
  min: number,
  max: number,
): T {
  const storedValue = localStorage.getItem(key)
  if (storedValue === null || storedValue.trim() === '') return fallback
  const value = Number(storedValue)
  return Number.isInteger(value) && value >= min && value <= max
    ? (value as T)
    : fallback
}

function DeckyChannelControls({
  deckyUi,
  channel,
  name,
  enabled,
  mode,
  level,
  onModeChange,
  onLevelChange,
}: {
  deckyUi: DeckyUiComponents
  channel: Channel
  name: string
  enabled: boolean
  mode: FocusMode
  level: FocusLevel
  onModeChange: (value: FocusMode) => void
  onLevelChange: (value: FocusLevel) => void
}) {
  const { PanelSection, PanelSectionRow, SliderField } = deckyUi
  const [selectedMode, setSelectedMode] = React.useState(mode)

  React.useEffect(() => setSelectedMode(mode), [mode])

  return (
    <PanelSection title={name}>
      <PanelSectionRow>
        <DeckyMenuSelector
          deckyUi={deckyUi}
          channel={channel}
          label="Current Focus Mode:"
          value={selectedMode}
          disabled={!enabled}
          onChange={(nextMode) => {
            setSelectedMode(nextMode)
            onModeChange(nextMode)
          }}
        />
      </PanelSectionRow>
      {selectedMode !== 0 && (
        <PanelSectionRow>
          <SliderField
            label="Adjustment level"
            tooltip="Set how strongly the selected focus mode is applied."
            value={level}
            min={1}
            max={3}
            step={1}
            notchCount={3}
            notchLabels={[
              { notchIndex: 0, value: 1, label: 'Weak' },
              { notchIndex: 1, value: 2, label: 'Medium' },
              { notchIndex: 2, value: 3, label: 'Strong' },
            ]}
            showValue={false}
            disabled={!enabled}
            onChange={(value) => onLevelChange(value as FocusLevel)}
          />
        </PanelSectionRow>
      )}
    </PanelSection>
  )
}

function DeckyMenuSelector({
  deckyUi,
  channel,
  label,
  value,
  disabled,
  onChange,
}: {
  deckyUi: DeckyUiComponents
  channel: Channel
  label: string
  value: FocusMode
  disabled: boolean
  onChange: (value: FocusMode) => void
}) {
  const { DialogButton, Menu, MenuItem, showContextMenu } = deckyUi
  const selectorRef = React.useRef<HTMLDivElement>(null)

  React.useEffect(() => {
    if (pendingModeMenuRestore?.channel !== channel) return
    const focusTarget =
      selectorRef.current?.querySelector<HTMLElement>('button') ?? null
    if (!focusTarget) return
    const scrollContainer = findScrollableAncestor(focusTarget)
    focusTarget.focus({ preventScroll: true })
    if (scrollContainer && pendingModeMenuRestore.scrollTop !== undefined) {
      scrollContainer.scrollTop = pendingModeMenuRestore.scrollTop
    }
    pendingModeMenuRestore = undefined
  }, [channel])

  const showMenu = () => {
    const focusTarget =
      selectorRef.current?.querySelector<HTMLElement>('button') ?? null
    const scrollContainer = focusTarget
      ? findScrollableAncestor(focusTarget)
      : null
    const scrollTop = scrollContainer?.scrollTop
    let contextMenu: DeckyContextMenu | undefined
    const closeMenu = () => {
      modeMenuOpen = false
      pendingModeMenuRestore = { channel, scrollTop }
      contextMenu?.Hide()
      window.setTimeout(() => {
        if (!focusTarget?.isConnected) return
        focusTarget.focus({ preventScroll: true })
        if (scrollContainer && scrollTop !== undefined) {
          scrollContainer.scrollTop = scrollTop
        }
        pendingModeMenuRestore = undefined
      }, 0)
    }
    modeMenuOpen = true
    contextMenu = showContextMenu(
      <Menu
        label="Select Ear Tuner Area You'd Like To Boost"
        cancelText="Return"
        onCancel={closeMenu}
      >
        {FOCUS_MODE_MENU_OPTIONS.map((option) => (
          <MenuItem
            key={option.value}
            selected={option.value === value}
            onClick={() => {
              onChange(option.value)
              closeMenu()
            }}
          >
            {option.label}
          </MenuItem>
        ))}
      </Menu>,
      focusTarget ?? undefined,
    )
  }

  return (
    <div ref={selectorRef} className="earTunerModeSelector">
      <div className="earTunerSelectedMode" aria-live="polite">
        <span>{label}</span>
        <strong>
          {
            FOCUS_MODE_MENU_OPTIONS.find((option) => option.value === value)
              ?.label
          }
        </strong>
      </div>
      <DialogButton
        className="earTunerChangeModeButton"
        disabled={disabled}
        onClick={showMenu}
      >
        Change
      </DialogButton>
    </div>
  )
}

function SamplePlayer({
  deckyUi,
  enabled,
}: {
  deckyUi: DeckyUiComponents
  enabled: boolean
}) {
  const { DialogButton, PanelSectionRow } = deckyUi
  const [playing, setPlaying] = React.useState(false)
  const [playbackError, setPlaybackError] = React.useState(false)

  React.useEffect(() => {
    const audio = getAuditionAudio()
    const updatePlaying = () => setPlaying(!audio.paused)
    const reportError = () => setPlaybackError(true)
    audio.addEventListener('play', updatePlaying)
    audio.addEventListener('pause', updatePlaying)
    audio.addEventListener('ended', updatePlaying)
    audio.addEventListener('error', reportError)
    updatePlaying()

    return () => {
      audio.removeEventListener('play', updatePlaying)
      audio.removeEventListener('pause', updatePlaying)
      audio.removeEventListener('ended', updatePlaying)
      audio.removeEventListener('error', reportError)
    }
  }, [])

  const togglePlayback = async () => {
    const audio = getAuditionAudio()
    if (!audio.paused) {
      stopAuditionSample()
      return
    }
    setPlaybackError(false)
    try {
      await audio.play()
    } catch (error) {
      setPlaybackError(true)
      console.error('Could not play the audition sample:', error)
    }
  }

  const playbackActionLabel = playing ? STOP_SAMPLE_LABEL : PLAY_SAMPLE_LABEL

  return (
    <div className="earTunerSamplePreview">
      <PanelSectionRow>
        <div className="earTunerSampleControl">
          <DialogButton
            className="earTunerSampleButton"
            disabled={!enabled && !playing}
            onClick={() => void togglePlayback()}
          >
            {playbackActionLabel}
          </DialogButton>
        </div>
      </PanelSectionRow>
      {playbackError && (
        <div className="sampleError" role="alert">
          Sample unavailable. Try again.
        </div>
      )}
    </div>
  )
}

export default App
export function App({
  deckyUi,
  onOpenInfo,
  quickAccessVisible = true,
}: {
  deckyUi: DeckyUiComponents
  onOpenInfo?: () => void
  quickAccessVisible?: boolean
}) {
  const initialSettings = latestEarTunerSettings
  const [enabled, setEnabled] = React.useState(
    () =>
      initialSettings?.enabled ??
      localStorage.getItem('focus-enabled') === 'true',
  )
  const [separateChannels, setSeparateChannels] = React.useState(
    () =>
      initialSettings?.separate_channels ??
      localStorage.getItem('focus-separate-channels') === 'true',
  )
  const [leftMode, setLeftMode] = React.useState<FocusMode>(
    () =>
      initialSettings?.left_mode ??
      readStoredNumber('focus-mode-left', 2, 0, 4),
  )
  const [rightMode, setRightMode] = React.useState<FocusMode>(
    () =>
      initialSettings?.right_mode ??
      readStoredNumber('focus-mode-right', 2, 0, 4),
  )
  const [leftLevel, setLeftLevel] = React.useState<FocusLevel>(
    () =>
      initialSettings?.left_level ??
      readStoredNumber('focus-level-left', 2, 1, 3),
  )
  const [rightLevel, setRightLevel] = React.useState<FocusLevel>(
    () =>
      initialSettings?.right_level ??
      readStoredNumber('focus-level-right', 2, 1, 3),
  )
  const [hydrated, setHydrated] = React.useState(false)
  const [settingsLoadFailed, setSettingsLoadFailed] = React.useState(false)
  const [settingsChanged, setSettingsChanged] = React.useState(false)
  const [settingsError, setSettingsError] = React.useState<string | null>(null)
  const restoredSettings = React.useRef<EarTunerSettings | null>(null)
  const pendingSaveTimeout = React.useRef<number | undefined>(undefined)
  const leftModeRef = React.useRef(leftMode)
  const rightModeRef = React.useRef(rightMode)
  const guideButtonRef = React.useRef<HTMLDivElement>(null)

  React.useEffect(() => {
    if (!quickAccessVisible || !hydrated || !pendingGuideRestore) return
    const focusTarget =
      guideButtonRef.current?.querySelector<HTMLElement>('button') ?? null
    if (!focusTarget) return
    const scrollContainer = findScrollableAncestor(focusTarget)
    focusTarget.focus({ preventScroll: true })
    if (scrollContainer && pendingGuideRestore.scrollTop !== undefined) {
      scrollContainer.scrollTop = pendingGuideRestore.scrollTop
    }
    pendingGuideRestore = undefined
  }, [hydrated, quickAccessVisible])

  const openInfo = () => {
    if (!onOpenInfo) return
    const focusTarget =
      guideButtonRef.current?.querySelector<HTMLElement>('button') ?? null
    const scrollContainer = focusTarget
      ? findScrollableAncestor(focusTarget)
      : null
    pendingGuideRestore = { scrollTop: scrollContainer?.scrollTop }
    onOpenInfo()
  }

  React.useEffect(() => {
    let active = true
    getDeckyPersistence()
      .then((persistence) => persistence?.get())
      .then((saved) => {
        if (!active || !saved) return
        const current = latestEarTunerSettings ?? saved
        confirmedEarTunerSettings ??= saved
        latestEarTunerSettings = current
        setEnabled(current.enabled)
        setSeparateChannels(current.separate_channels)
        setLeftMode(current.left_mode)
        leftModeRef.current = current.left_mode
        setLeftLevel(current.left_level)
        setRightMode(current.right_mode)
        rightModeRef.current = current.right_mode
        setRightLevel(current.right_level)
      })
      .catch((error) => {
        if (active) setSettingsLoadFailed(true)
        console.error('Could not load Ear Tuner settings:', error)
      })
      .finally(() => {
        if (active) setHydrated(true)
      })
    return () => {
      active = false
    }
  }, [])

  const saveSettings = (value: EarTunerSettings) => {
    setSettingsError(null)
    void saveEarTunerSettings(value).catch((error) => {
      console.error('Could not save Ear Tuner settings:', error)
      if (latestEarTunerSettings !== value) return
      const confirmed = confirmedEarTunerSettings
      if (confirmed) {
        restoredSettings.current = confirmed
        latestEarTunerSettings = confirmed
        cacheSettings(confirmed)
        setEnabled(confirmed.enabled)
        setSeparateChannels(confirmed.separate_channels)
        leftModeRef.current = confirmed.left_mode
        rightModeRef.current = confirmed.right_mode
        setLeftMode(confirmed.left_mode)
        setRightMode(confirmed.right_mode)
        setLeftLevel(confirmed.left_level)
        setRightLevel(confirmed.right_level)
      }
      setSettingsError(
        'Settings were not confirmed. Audio may still use the previous settings. Try your change again.',
      )
    })
  }

  React.useEffect(() => {
    if (!hydrated || (settingsLoadFailed && !settingsChanged)) return
    const value: EarTunerSettings = {
      enabled,
      separate_channels: separateChannels,
      left_mode: leftModeRef.current,
      left_level: leftLevel,
      right_mode: rightModeRef.current,
      right_level: rightLevel,
    }
    cacheSettings(value)
    latestEarTunerSettings = value
    if (restoredSettings.current) {
      const restored = sameSettings(value, restoredSettings.current)
      restoredSettings.current = null
      if (restored) return
    }
    const timeout = window.setTimeout(() => {
      saveSettings(value)
    }, 150)
    pendingSaveTimeout.current = timeout
    return () => window.clearTimeout(timeout)
  }, [
    hydrated,
    enabled,
    separateChannels,
    leftLevel,
    rightLevel,
    settingsLoadFailed,
    settingsChanged,
  ])

  React.useEffect(() => {
    if (!hydrated) return
    const params = new URLSearchParams({ focus_bypass: '0' })
    fetch(`${NATIVE_CONTROL_API_URL}?${params}`).catch((error) =>
      console.error('Could not restore audio processing:', error),
    )
  }, [hydrated])

  React.useEffect(() => {
    if (!quickAccessVisible && !modeMenuOpen) stopAuditionSample()
  }, [quickAccessVisible])

  React.useEffect(
    () => () => {
      if (!modeMenuOpen) stopAuditionSample()
    },
    [],
  )

  const applyMode = (channel: 'left' | 'right', mode: FocusMode) => {
    window.clearTimeout(pendingSaveTimeout.current)
    if (channel === 'left') {
      leftModeRef.current = mode
      setLeftMode(mode)
    } else {
      rightModeRef.current = mode
      setRightMode(mode)
    }
    const value: EarTunerSettings = {
      enabled,
      separate_channels: separateChannels,
      left_mode: leftModeRef.current,
      left_level: leftLevel,
      right_mode: rightModeRef.current,
      right_level: rightLevel,
    }
    latestEarTunerSettings = value
    localStorage.setItem(`focus-mode-${channel}`, String(mode))
    saveSettings(value)
  }

  const {
    ButtonItem,
    Focusable,
    GamepadButton,
    PanelSection,
    PanelSectionRow,
    ToggleField,
  } = deckyUi
  if (!hydrated) {
    return (
      <div className="earTunerPlugin deckyPanel">
        <PanelSection title="Ear Tuner">
          <div className="headphoneNotice" role="status" aria-live="polite">
            Loading Ear Tuner settings...
          </div>
        </PanelSection>
      </div>
    )
  }
  return (
    <Focusable
      actionDescriptionMap={{
        [GamepadButton.BUMPER_RIGHT]: 'Next page',
      }}
      onButtonDown={(event) => {
        if (event.detail.button === GamepadButton.BUMPER_RIGHT) {
          event.stopPropagation()
          openInfo()
        }
      }}
    >
      <div className="earTunerPlugin deckyPanel">
        <PanelSection>
          <div className="headphoneNotice" role="note">
            {HEADPHONE_ADVISORY}
          </div>
          <p className="earTunerIntroduction">{PRODUCT_SUMMARY}</p>
          <ToggleField
            label="Enable Ear Tuner"
            description={enabled ? 'Enabled' : 'Disabled'}
            tooltip="Enable Ear Tuner for the current audio output."
            checked={enabled}
            onChange={(value) => {
              setSettingsChanged(true)
              setEnabled(value)
            }}
          />
          {settingsError && (
            <div className="sampleError" role="alert">
              {settingsError}
            </div>
          )}
        </PanelSection>
        <div className="earTunerUsageInstructions">
          <p>Play audio from a game, or use the sample below while tuning.</p>
        </div>
        <PanelSection title="Listening Sample">
          <SamplePlayer deckyUi={deckyUi} enabled={enabled} />
        </PanelSection>
        <div className="earTunerUsageInstructions">
          <p>
            Choose whether to adjust both ears together or each ear separately.
            Then select a focus mode and strength.
          </p>
        </div>
        <PanelSection>
          <ToggleField
            label={SEPARATE_EARS_LABEL}
            tooltip="Use different focus modes and strengths for the left and right ears."
            checked={separateChannels}
            disabled={!enabled}
            onChange={(value) => {
              setSettingsChanged(true)
              setSeparateChannels(value)
            }}
          />
        </PanelSection>
        <DeckyChannelControls
          deckyUi={deckyUi}
          channel="left"
          name={separateChannels ? 'Left ear' : 'Both ears'}
          enabled={enabled}
          mode={leftMode}
          level={leftLevel}
          onModeChange={(mode) => applyMode('left', mode)}
          onLevelChange={(value) => {
            setSettingsChanged(true)
            setLeftLevel(value)
          }}
        />
        {separateChannels && (
          <DeckyChannelControls
            deckyUi={deckyUi}
            channel="right"
            name="Right ear"
            enabled={enabled}
            mode={rightMode}
            level={rightLevel}
            onModeChange={(mode) => applyMode('right', mode)}
            onLevelChange={(value) => {
              setSettingsChanged(true)
              setRightLevel(value)
            }}
          />
        )}
        <PanelSection title="Help & Project">
          <PanelSectionRow>
            <p className="helpActionDescription">
              Usage, help, and project links.
            </p>
          </PanelSectionRow>
          <div ref={guideButtonRef}>
            <ButtonItem
              tooltip="Open the Ear Tuner guide."
              layout="below"
              onClick={openInfo}
            >
              Ear Tuner Guide
            </ButtonItem>
          </div>
        </PanelSection>
      </div>
    </Focusable>
  )
}
export type { EarTunerSettings, DeckyUiComponents }
