declare module 'which' {
  interface WhichOptions {
    nothrow?: boolean
    all?: boolean
    path?: string
    pathExt?: string
    delimiter?: string
  }

  function which(cmd: string, options?: WhichOptions): Promise<string | null>
  namespace which {
    function sync(cmd: string, options?: WhichOptions): string | null
  }
  export default which
}
