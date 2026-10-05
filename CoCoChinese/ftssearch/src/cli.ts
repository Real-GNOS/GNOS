#!/usr/bin/env node
import { Ftssearch } from './index.js'
import { startServer } from './server.js'
import {
  resolveConfigPath,
  loadConfigFile,
  getCacheOptions,
  SEARCH_PATHS,
} from './config.js'
import type { ColumnMapping } from './types.js'
import type { FtssearchConfigFile } from './config.js'

const DEFAULT_COLUMNS: Record<string, ColumnMapping[]> = {
  videos: [
    { column: 'title',       weight: 'A' },
    { column: 'tags',        weight: 'B', isArray: true },
    { column: 'category',    weight: 'B' },
    { column: 'author',      weight: 'C' },
    { column: 'description', weight: 'D' },
    { column: 'introduction',weight: 'D' },
  ],
  posts: [
    { column: 'title',   weight: 'A' },
    { column: 'content', weight: 'D' },
    { column: 'author',  weight: 'C' },
  ],
  articles: [
    { column: 'title',    weight: 'A' },
    { column: 'body',     weight: 'D' },
    { column: 'summary',  weight: 'C' },
    { column: 'author',   weight: 'C' },
    { column: 'tags',     weight: 'B', isArray: true },
  ],
  products: [
    { column: 'name',        weight: 'A' },
    { column: 'description', weight: 'D' },
    { column: 'category',    weight: 'B' },
    { column: 'brand',       weight: 'C' },
    { column: 'tags',        weight: 'B', isArray: true },
  ],
}

function parseColumns(env: string | undefined): ColumnMapping[] | null {
  if (!env) return null
  try { return JSON.parse(env) as ColumnMapping[] }
  catch { return null }
}

function printConfig(cfg: FtssearchConfigFile): void {
  if (cfg.server) {
    console.log(`  Port:     ${cfg.server.port ?? 3001}`)
    console.log(`  Host:     ${cfg.server.host ?? '0.0.0.0'}`)
  }
  console.log(`  Table:    ${cfg.table ?? 'videos'}`)
  console.log(`  Language: ${cfg.language ?? 'simple'}`)
  console.log(`  Redis:    ${cfg.cache?.redis?.url || '(memory only)'}`)
  console.log(`  Columns:  ${(cfg.columns ?? []).map(c => `${c.column}(${c.weight})`).join(', ')}`)
}

async function main(): Promise<void> {
  const args = process.argv.slice(2)
  const command = args[0]

  let configPath: string | null = null
  const configIdx = args.indexOf('--config')
  if (configIdx !== -1 && args[configIdx + 1]) {
    configPath = resolveConfigPath(args[configIdx + 1])
  } else {
    configPath = resolveConfigPath()
  }

  let cfg: FtssearchConfigFile = {}
  if (configPath) {
    const loaded = loadConfigFile(configPath)
    if (loaded) {
      cfg = loaded
      console.log(`[Ftssearch] Loaded config: ${configPath}`)
    }
  } else {
    const cwd = process.cwd()
    console.log(`[Ftssearch] No config file found (searched: ${SEARCH_PATHS.map(p => `"${p}"`).join(', ')}) in ${cwd}`)
  }

  if (command === 'server' || command === 'serve' || !command) {
    const database = cfg.database ?? process.env.DATABASE_URL ?? process.env.PG_URL ?? 'postgresql://postgres:123@localhost:5432/cocokalo'
    const port = parseInt(args[1] ?? String(cfg.server?.port ?? process.env.FTSSEARCH_PORT ?? '3001'), 10) || 3001
    const host = cfg.server?.host ?? process.env.FTSSEARCH_HOST ?? '0.0.0.0'
    const table = cfg.table ?? process.env.FTSSEARCH_TABLE ?? 'videos'
    const language = cfg.language ?? process.env.FTSSEARCH_LANGUAGE ?? 'simple'

    let columns = parseColumns(process.env.FTSSEARCH_COLUMNS)
      ?? cfg.columns
      ?? DEFAULT_COLUMNS[table]
    if (!columns || !columns.length) columns = [{ column: 'title', weight: 'A' }]

    const cacheOptions = getCacheOptions(cfg.cache)
    if (cacheOptions !== false) {
      if (process.env.REDIS_URL) cacheOptions.redisUrl = process.env.REDIS_URL
      if (!cacheOptions.ttl && process.env.FTSSEARCH_CACHE_TTL) cacheOptions.ttl = parseInt(process.env.FTSSEARCH_CACHE_TTL, 10)
      if (!cacheOptions.maxEntries && process.env.FTSSEARCH_CACHE_MAX) cacheOptions.maxEntries = parseInt(process.env.FTSSEARCH_CACHE_MAX, 10)
    }

    console.log(`[Ftssearch] Starting server...`)
    printConfig({ ...cfg, server: { port, host }, table, language, columns, cache: cfg.cache })

    const fts = new Ftssearch(database, { cache: cacheOptions })

    const server = await startServer(fts, {
      port, host, table, columns, language, cors: true,
    })

    process.on('SIGINT', async () => {
      console.log('\n[Ftssearch] Shutting down...')
      server.close()
      await fts.close()
      process.exit(0)
    })

    process.on('SIGTERM', async () => {
      server.close()
      await fts.close()
      process.exit(0)
    })

    return
  }

  if (command === 'install') {
    const table = args[1] ?? cfg.table ?? process.env.FTSSEARCH_TABLE ?? 'videos'
    const database = cfg.database ?? process.env.DATABASE_URL ?? process.env.PG_URL ?? 'postgresql://postgres:123@localhost:5432/cocokalo'
    const language = cfg.language ?? process.env.FTSSEARCH_LANGUAGE ?? 'simple'

    let columns = parseColumns(process.env.FTSSEARCH_COLUMNS)
      ?? cfg.columns
      ?? DEFAULT_COLUMNS[table]
    if (!columns || !columns.length) columns = [{ column: 'title', weight: 'A' }]

    console.log(`[Ftssearch] Installing index on table "${table}"...`)
    const fts = new Ftssearch(database)
    await fts.install({ table, columns, language })
    console.log(`[Ftssearch] Index installed successfully`)
    await fts.close()
    return
  }

  if (command === 'uninstall') {
    const table = args[1] ?? cfg.table ?? process.env.FTSSEARCH_TABLE ?? 'videos'
    const database = cfg.database ?? process.env.DATABASE_URL ?? process.env.PG_URL ?? 'postgresql://postgres:123@localhost:5432/cocokalo'

    console.log(`[Ftssearch] Uninstalling index from table "${table}"...`)
    const fts = new Ftssearch(database)
    await fts.uninstall(table)
    console.log(`[Ftssearch] Index uninstalled successfully`)
    await fts.close()
    return
  }

  if (command === 'reindex') {
    const table = args[1] ?? cfg.table ?? process.env.FTSSEARCH_TABLE ?? 'videos'
    const database = cfg.database ?? process.env.DATABASE_URL ?? process.env.PG_URL ?? 'postgresql://postgres:123@localhost:5432/cocokalo'
    const language = cfg.language ?? process.env.FTSSEARCH_LANGUAGE ?? 'simple'

    let columns = parseColumns(process.env.FTSSEARCH_COLUMNS)
      ?? cfg.columns
      ?? DEFAULT_COLUMNS[table]
    if (!columns || !columns.length) columns = [{ column: 'title', weight: 'A' }]

    console.log(`[Ftssearch] Reindexing table "${table}"...`)
    const fts = new Ftssearch(database)
    await fts.reindex(table, columns, language)
    console.log(`[Ftssearch] Reindex completed`)
    await fts.close()
    return
  }

  if (command === 'init') {
    const path = args[1] || 'ftssearch.config.json'
    const sample: FtssearchConfigFile = {
      database: 'postgresql://user:pass@localhost:5432/mydb',
      table: 'videos',
      language: 'simple',
      columns: [
        { column: 'title',       weight: 'A' },
        { column: 'tags',        weight: 'B', isArray: true },
        { column: 'category',    weight: 'B' },
        { column: 'author',      weight: 'C' },
        { column: 'description', weight: 'D' },
        { column: 'introduction',weight: 'D' },
      ],
      cache: {
        redis: { url: 'redis://localhost:6379' },
        ttl: 60,
        maxEntries: 1000,
      },
      server: {
        port: 3001,
        host: '0.0.0.0',
      },
    }
    const { writeFileSync } = await import('fs')
    writeFileSync(path, JSON.stringify(sample, null, 2) + '\n', 'utf-8')
    console.log(`[Ftssearch] Created config file: ${path}`)
    return
  }

  console.log(`
Ftssearch - PostgreSQL Full-Text Search Engine

USAGE:
  ftssearch server [port]    Start the standalone API server
  ftssearch install [table]  Install search index on a table
  ftssearch uninstall [table] Remove search index from a table
  ftssearch reindex [table]  Rebuild search index
  ftssearch init [file]      Generate a sample config file

CONFIG FILE (auto-discovered):
  ${SEARCH_PATHS.join('\n  ')}

  Or pass explicit path: --config /path/to/config.json

EXAMPLE CONFIG:
  {
    "database": "postgresql://user:pass@localhost:5432/mydb",
    "table": "videos",
    "language": "simple",
    "columns": [
      { "column": "title", "weight": "A" },
      { "column": "tags",  "weight": "B", "isArray": true }
    ],
    "cache": {
      "redis": { "url": "redis://localhost:6379" },
      "ttl": 60,
      "maxEntries": 1000
    },
    "server": {
      "port": 3001,
      "host": "0.0.0.0"
    }
  }
`)
}

main().catch((err) => {
  console.error('[Ftssearch] Fatal error:', err)
  process.exit(1)
})
