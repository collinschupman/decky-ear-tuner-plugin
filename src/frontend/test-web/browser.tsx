import React from 'react'
import { createRoot } from 'react-dom/client'

import { App, DeckyInfoPage } from '../decky/app'
import './preview.css'
import { DeckyPreviewShell, previewDeckyUi } from './preview_ui'

function BrowserPreview() {
  const [showInfo, setShowInfo] = React.useState(false)

  return (
    <DeckyPreviewShell
      title={showInfo ? 'Ear Tuner Guide' : 'Ear Tuner'}
      onBack={showInfo ? () => setShowInfo(false) : undefined}
    >
      {showInfo ? (
        <DeckyInfoPage
          deckyUi={previewDeckyUi}
          onBack={() => setShowInfo(false)}
        />
      ) : (
        <App deckyUi={previewDeckyUi} onOpenInfo={() => setShowInfo(true)} />
      )}
    </DeckyPreviewShell>
  )
}

document.body.classList.add('earTunerStandalone')
createRoot(document.getElementById('root')!).render(<BrowserPreview />)
