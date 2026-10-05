import { existsSync, readFileSync } from 'fs'
import { join, resolve } from 'path'
import { createRequire } from 'module'
import type { ColumnMapping, CacheOptions } from './types.js'

const _require = createRequire(import.meta.url)

export interface ServerSection {
  port?: number
  host?: string
}

export interface CacheSection {
  redis?: { url?: string }
  ttl?: number
  maxEntries?: number
}

export interface FtssearchConfigFile {
  database?: string
  pool?: { max?: number }
  table?: string
  language?: string
  columns?: ColumnMapping[]
  cache?: CacheSection
  server?: ServerSection
}

const SEARCH_PATHS = [
  'ftssearch.config.json',
  'ftssearch.config.js',
  '.ftssearchrc',
  'ftssearch.json',
]

function findConfigFile(startDir?: string): string | null {
  const dirs = [
    startDir && resolve(startDir),
    process.cwd(),
    process.cwd().endsWith('ftssearch') ? resolve(process.cwd(), '..') : null,
  ].filter(Boolean) as string[]

  for (const dir of dirs) {
    for (const name of SEARCH_PATHS) {
      const full = join(dir, name)
      if (existsSync(full)) return full
    }
  }
  return null
}

function loadJsonConfig(path: string): FtssearchConfigFile | null {
  try {
    const raw = readFileSync(path, 'utf-8')
    return JSON.parse(raw) as FtssearchConfigFile
  } catch (err) {
    console.error(`[Ftssearch] Failed to load config file "${path}":`, err)
    return null
  }
}

function loadJsConfig(path: string): FtssearchConfigFile | null {
  try {
    const mod = _require(path) as { default?: FtssearchConfigFile; config?: FtssearchConfigFile }
    return mod.default ?? mod.config ?? (mod as unknown as FtssearchConfigFile)
  } catch (err) {
    console.error(`[Ftssearch] Failed to load JS config "${path}":`, err)
    return null
  }
}

export function resolveConfigPath(userPath?: string): string | null {
  if (userPath) {
    const full = resolve(userPath)
    if (existsSync(full)) return full
    console.error(`[Ftssearch] Config file not found: ${full}`)
    return null
  }
  return findConfigFile()
}

export function loadConfigFile(path: string): FtssearchConfigFile | null {
  if (path.endsWith('.js')) return loadJsConfig(path)
  return loadJsonConfig(path)
}

export function getCacheOptions(section?: CacheSection): CacheOptions | false {
  if (!section) return {}
  return {
    redisUrl: section.redis?.url,
    ttl: section.ttl,
    maxEntries: section.maxEntries,
  }
}

export { SEARCH_PATHS }
