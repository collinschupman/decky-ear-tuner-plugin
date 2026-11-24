import React from 'react'

import { stopAuditionSample } from '../decky/app'

type AudioStatus = 'unknown' | 'unsupported' | 'running' | 'stopped'

export function AudioServiceControl({
  children,
}: {
  children: React.ReactNode
}) {
  const [status, setStatus] = React.useState<AudioStatus>('unknown')
  const [busy, setBusy] = React.useState(false)
  const [error, setError] = React.useState('')

  React.useEffect(() => {
    if (busy || status === 'unsupported') return
    let active = true
    const refresh = async () => {
      try {
        const response = await fetch('/api/audio-service')
        if (!active) return
        if (response.status === 404) {
          setStatus('unsupported')
          return
        }
        if (!response.ok) throw new Error(`HTTP ${response.status}`)
        const value: unknown = await response.json()
        if (
          !value ||
          typeof value !== 'object' ||
          !('running' in value) ||
          typeof value.running !== 'boolean'
        ) {
          throw new Error('Invalid audio service status')
        }
        if (active) {
          setStatus(value.running ? 'running' : 'stopped')
          setError((current) =>
            current.startsWith('Could not check audio service:') ? '' : current,
          )
        }
      } catch (reason) {
        if (active) setError(`Could not check audio service: ${String(reason)}`)
      }
    }
    void refresh()
    const timer = window.setInterval(refresh, 3000)
    return () => {
      active = false
      window.clearInterval(timer)
    }
  }, [busy, status === 'unsupported'])

  async function setRunning(running: boolean) {
    setBusy(true)
    setError('')
    try {
      const response = await fetch('/api/audio-service', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ running }),
      })
      if (!response.ok) throw new Error(`HTTP ${response.status}`)
      setStatus(running ? 'running' : 'stopped')
      if (!running) stopAuditionSample()
    } catch (reason) {
      setError(
        `Could not ${running ? 'start' : 'stop'} audio: ${String(reason)}`,
      )
    } finally {
      setBusy(false)
    }
  }

  return (
    <>
      {status !== 'unsupported' && (
        <section className="deckyPreviewSection">
          <h2>Audio service</h2>
          <div className="deckyPreviewSectionContent">
            <button
              className="deckyPreviewButtonItem"
              disabled={busy || status === 'unknown'}
              onClick={() => void setRunning(status !== 'running')}
            >
              {busy
                ? 'Changing audio service...'
                : status === 'running'
                  ? 'Stop Audio'
                  : 'Start Audio'}
            </button>
            <small>
              {status === 'stopped'
                ? 'Audio is stopped or unavailable. Start Audio to try again.'
                : 'Stop audio to release the system audio capture.'}
            </small>
            {error && <small role="alert">{error}</small>}
          </div>
        </section>
      )}
      {status !== 'stopped' && children}
    </>
  )
}
