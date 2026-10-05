let _initializing: Promise<void> | null = null

export default defineNitroPlugin(async () => {
  if (_initializing) return _initializing

  _initializing = (async () => {
    try {
      await initPgTables()
      console.log('PostgreSQL ready')

      const { reindexSearch } = await import('~/server/utils/search')
      await reindexSearch().catch((err: any) =>
        console.error('Search reindex error (non-fatal):', err.message)
      )
    } catch (err: any) {
      console.error('PostgreSQL init error:', err)
      _initializing = null
    }
  })()

  return _initializing
})
