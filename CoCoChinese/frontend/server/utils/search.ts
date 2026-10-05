import { Ftssearch, type ColumnMapping, loadConfigFile, getCacheOptions } from 'ftssearch'
import { getPgPool } from './pg'
import { existsSync } from 'fs'
import { resolve } from 'path'

function findConfigPath(): string | undefined {
  const candidates = [
    resolve('ftssearch.config.json'),
    resolve('../ftssearch.config.json'),
  ]
  for (const p of candidates) {
    if (existsSync(p)) return p
  }
}

const configPath = findConfigPath()
const configFile = configPath ? loadConfigFile(configPath) : null

const columns = (configFile?.columns ?? [
  { column: 'title',       weight: 'A' },
  { column: 'tags',        weight: 'B', isArray: true },
  { column: 'category',    weight: 'B' },
  { column: 'author',      weight: 'C' },
  { column: 'description', weight: 'D' },
  { column: 'introduction',weight: 'D' },
]) as ColumnMapping[]

let _fts: Ftssearch | null = null

function getFts(): Ftssearch {
  if (!_fts) {
    _fts = new Ftssearch(getPgPool(), {
      cache: configFile ? getCacheOptions(configFile.cache) : {
        redisUrl: '',
        ttl: 60,
        maxEntries: 2000,
      },
    })
  }
  return _fts
}

export async function searchVideos(q: string, from = 0, size = 20) {
  const result = await getFts().search('videos', columns, q, 'simple', {
    limit: size,
    offset: from,
  })
  return {
    total: result.total,
    results: result.hits.map(hit => ({
      ...hit.source,
      id: (hit.source as Record<string, unknown>).slug || (hit.source as Record<string, unknown>).id,
      _score: hit.rank,
    })),
  }
}

export async function reindexSearch() {
  await getFts().reindex('videos', columns)
  console.log('Search index rebuilt')
}

export async function indexVideo(_video: unknown) {}
export async function updateVideo(_slug: string, _updates: unknown) {}
export async function deleteVideo(_slug: string) {}
