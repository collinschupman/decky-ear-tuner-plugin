import type {
  ButtonItemProps,
  DialogButtonProps,
  FocusableProps,
  PanelSectionProps,
  PanelSectionRowProps,
  SliderFieldProps,
  ToggleFieldProps,
} from '@decky/ui'
import React from 'react'

import type { DeckyUiComponents } from '../decky/app'

let updateContextMenu: ((menu: React.ReactNode | null) => void) | undefined

function PanelSection({ title, children }: PanelSectionProps) {
  return (
    <section className="deckyPreviewSection">
      {title && <h2>{title}</h2>}
      <div className="deckyPreviewSectionContent">{children}</div>
    </section>
  )
}

function PanelSectionRow({ children }: PanelSectionRowProps) {
  return <div className="deckyPreviewRow">{children}</div>
}

function Focusable({ children, className }: FocusableProps) {
  return <div className={className}>{children}</div>
}

function DialogButton({
  children,
  className,
  disabled,
  onClick,
}: DialogButtonProps) {
  return (
    <button
      className={`deckyPreviewDialogButton ${className ?? ''}`}
      disabled={disabled}
      onClick={(event) => onClick?.(event.nativeEvent)}
    >
      {children}
    </button>
  )
}

function ToggleField({
  label,
  description,
  checked,
  disabled,
  onChange,
}: ToggleFieldProps) {
  return (
    <label className={`deckyPreviewToggle ${disabled ? 'disabled' : ''}`}>
      <span className="deckyPreviewItemText">
        <strong>{label}</strong>
        {description && <small>{description}</small>}
      </span>
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled}
        onChange={(event) => onChange?.(event.target.checked)}
      />
      <span className="deckyPreviewToggleTrack" aria-hidden="true" />
    </label>
  )
}

function SliderField({
  label,
  value,
  min = 0,
  max = 100,
  step = 1,
  notchLabels,
  disabled,
  onChange,
}: SliderFieldProps) {
  const progress = ((value - min) / (max - min)) * 100
  return (
    <div className={`deckyPreviewSlider ${disabled ? 'disabled' : ''}`}>
      <strong>{label}</strong>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        disabled={disabled}
        style={{ '--slider-progress': `${progress}%` } as React.CSSProperties}
        onChange={(event) => onChange?.(Number(event.target.value))}
      />
      {notchLabels && (
        <div className="deckyPreviewNotches">
          {notchLabels.map((notch) => (
            <span key={notch.notchIndex}>{notch.label}</span>
          ))}
        </div>
      )}
    </div>
  )
}

function ButtonItem({ label, children, disabled, onClick }: ButtonItemProps) {
  return (
    <button
      className="deckyPreviewButtonItem"
      disabled={disabled}
      onClick={(event) => onClick?.(event.nativeEvent)}
    >
      <span className="deckyPreviewItemText">
        <strong>{children}</strong>
        {label && <small>{label}</small>}
      </span>
      <span aria-hidden="true">›</span>
    </button>
  )
}

function Menu({
  label,
  cancelText,
  onCancel,
  children,
}: {
  label: string
  cancelText?: string
  onCancel?: () => void
  children?: React.ReactNode
}) {
  return (
    <div className="deckyPreviewMenu" role="dialog" aria-label={label}>
      <h2>{label}</h2>
      <div>{children}</div>
      <button onClick={onCancel}>{cancelText ?? 'Cancel'}</button>
    </div>
  )
}

function MenuItem({
  children,
  selected,
  onClick,
}: {
  children?: React.ReactNode
  selected?: boolean
  onClick?: (event: Event) => void
}) {
  return (
    <button
      className={selected ? 'selected' : ''}
      onClick={(event) => onClick?.(event.nativeEvent)}
    >
      <span>{children}</span>
      {selected && <span aria-label="Selected">✓</span>}
    </button>
  )
}

export const previewDeckyUi: DeckyUiComponents = {
  ButtonItem,
  DialogButton,
  Focusable,
  GamepadButton: {
    BUMPER_LEFT: 4,
    BUMPER_RIGHT: 5,
  },
  Menu,
  MenuItem,
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  showContextMenu(children) {
    updateContextMenu?.(children)
    return { Hide: () => updateContextMenu?.(null) }
  },
}

export function DeckyPreviewShell({
  title,
  onBack,
  children,
}: {
  title: string
  onBack?: () => void
  children: React.ReactNode
}) {
  const [contextMenu, setContextMenu] = React.useState<React.ReactNode | null>(
    null,
  )

  React.useEffect(() => {
    updateContextMenu = setContextMenu
    return () => {
      updateContextMenu = undefined
    }
  }, [])

  return (
    <main className="deckyPreviewViewport">
      <div className="deckyPreviewBackdrop" aria-hidden="true">
        <span>LIBRARY</span>
        <strong>Ear Tuner UI Preview</strong>
      </div>
      <aside className="deckyPreviewQuickAccess" aria-label="Quick Access">
        <header>
          {onBack ? (
            <button aria-label="Back" onClick={onBack}>
              ‹
            </button>
          ) : (
            <span className="deckyPreviewHeaderSpacer" />
          )}
          <h1>{title}</h1>
          <time>12:45</time>
        </header>
        <nav aria-label="Quick Access tabs">
          <span>●</span>
          <span>♬</span>
          <span>⚙</span>
          <span className="selected">◉</span>
          <span>⋯</span>
        </nav>
        <div className="deckyPreviewContent">{children}</div>
        <footer>
          <span>
            <kbd>B</kbd> Back
          </span>
          <span>
            <kbd>A</kbd> Select
          </span>
        </footer>
        {contextMenu && (
          <div className="deckyPreviewMenuScrim">{contextMenu}</div>
        )}
      </aside>
    </main>
  )
}
