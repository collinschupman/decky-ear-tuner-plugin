import { definePlugin, useQuickAccessVisible } from '@decky/api'
import {
  ButtonItem,
  DialogButton,
  Focusable,
  GamepadButton,
  Menu,
  MenuItem,
  PanelSection,
  PanelSectionRow,
  SliderField,
  scrollPanelClasses,
  showContextMenu,
  ToggleField,
} from '@decky/ui'
import React from 'react'

import { DeckyPluginContent, stopAuditionSample } from './app'
import styles from './styles.css'

const deckyUi = {
  ButtonItem,
  DialogButton,
  Focusable,
  GamepadButton,
  Menu,
  MenuItem,
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  showContextMenu,
}

function EarTunerIcon() {
  return (
    <svg
      aria-hidden="true"
      fill="none"
      viewBox="0 0 24 24"
      style={{ height: '1em', width: '1em' }}
    >
      <path
        d="M4 13v-1a8 8 0 0 1 16 0v1"
        stroke="currentColor"
        strokeLinecap="round"
        strokeWidth="2.2"
      />
      <path
        d="M3 14.5A2.5 2.5 0 0 1 5.5 12H7v8H5.5A2.5 2.5 0 0 1 3 17.5v-3Zm14-2.5h1.5a2.5 2.5 0 0 1 2.5 2.5v3a2.5 2.5 0 0 1-2.5 2.5H17v-8Z"
        fill="currentColor"
      />
      <path
        d="m9 16 1.2-2.2 1.6 4.4 1.4-3.3.8 1.1h1"
        stroke="currentColor"
        strokeLinecap="round"
        strokeLinejoin="round"
        strokeWidth="1.4"
      />
    </svg>
  )
}

function DeckyApp() {
  const quickAccessVisible = useQuickAccessVisible()

  return (
    <Focusable>
      <Focusable className="earTunerDeckyFocusBoundary">
        <DeckyPluginContent
          deckyUi={deckyUi}
          quickAccessVisible={quickAccessVisible}
        />
      </Focusable>
      <style>{`
        .${scrollPanelClasses.ScrollPanel}:has(.earTunerDeckyFocusBoundary) {
          scroll-padding-top: 150px;
        }
      `}</style>
    </Focusable>
  )
}

export default definePlugin(() => {
  return {
    name: 'Ear Tuner',
    titleView: <div>Ear Tuner</div>,
    content: (
      <>
        <style>{styles}</style>
        <DeckyApp />
      </>
    ),
    icon: <EarTunerIcon />,
    onDismount() {
      stopAuditionSample()
    },
  }
})
