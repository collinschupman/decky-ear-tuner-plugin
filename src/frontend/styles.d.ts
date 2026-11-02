declare const __APP_VERSION__: string

declare module '*.css' {
  const styles: string
  export default styles
}

declare module '*.wav' {
  const url: string
  export default url
}
