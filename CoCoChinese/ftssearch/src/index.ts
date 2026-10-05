import { Pool } from 'pg'

import type {
  ColumnMapping,
  IndexConfig,
  SearchOptions,
  SearchResult,
  SearchHit,
  CacheStats,
  FtssearchOptions,
} from './types.js'
import {
  sqlCreateTriggerFunction,
  sqlCreateTrigger,
  sqlCreateTrigger2,
  sqlAddTsvColumn,
  sqlCreateIndex,
  sqlReindex,
  sqlDropTrigger,
  sqlDropFunction,
  sqlDropIndex,
  sqlDropTsvColumn,
  sqlSearch,
} from './queries.js'
import { SearchCache } from './cache.js'

const DEFAULT_SEARCH_OPTIONS: Required<SearchOptions> = {
  limit: 20,
  offset: 0,
  weights: [0.1, 0.2, 0.4, 1.0],
  normalize: 32,
  fallbackToLike: true,
  cacheTTL: 60,
}

function resolveOptions(input?: SearchOptions): Required<SearchOptions> {
  return { ...DEFAULT_SEARCH_OPTIONS, ...input }
}

type PoolConfig = string | { connectionString?: string; pool?: Pool }

interface PoolInfo {
  pool: Pool
  owned: boolean
}

function resolvePool(config: PoolConfig): PoolInfo {
  if (typeof config === 'string') {
    return { pool: new Pool({ connectionString: config }), owned: true }
  }
  if (config && typeof config === 'object') {
    if ('query' in config && 'connect' in config && 'end' in config) {
      return { pool: config as unknown as Pool, owned: false }
    }
    if ('pool' in config && config.pool) {
      return { pool: config.pool, owned: false }
    }
    if ('connectionString' in config && config.connectionString) {
      return { pool: new Pool({ connectionString: config.connectionString }), owned: true }
    }
  }
  return { pool: new Pool(), owned: true }
}

export class Ftssearch {
  private pool: Pool
  private owned: boolean
  private cache: SearchCache

  constructor(config: string | Pool | { connectionString?: string; pool?: Pool }, options?: FtssearchOptions) {
    const resolved = resolvePool(config as PoolConfig)
    this.pool = resolved.pool
    this.owned = resolved.owned

    const cacheOpts = options?.cache === false ? { enabled: false } : options?.cache
    this.cache = new SearchCache(cacheOpts)
  }

  async close(): Promise<void> {
    if (this.owned) {
      await this.pool.end()
    }
  }

  private escapeLike(s: string): string {
    return s.replace(/[%_\\]/g, '\\$&')
  }

  async install(config: IndexConfig): Promise<void> {
    const { table, columns, language = 'simple' } = config
    const client = await this.pool.connect()
    try {
      await client.query('BEGIN')
      await client.query(sqlAddTsvColumn(table))
      await client.query(sqlCreateIndex(table))
      await client.query(sqlCreateTriggerFunction(table, columns, language))
      await client.query(sqlCreateTrigger(table))
      await client.query(sqlCreateTrigger2(table))
      await client.query(sqlReindex(table, columns, language))
      await client.query('COMMIT')
    } catch (err) {
      await client.query('ROLLBACK')
      throw err
    } finally {
      client.release()
    }
  }

  async uninstall(table: string): Promise<void> {
    const client = await this.pool.connect()
    try {
      await client.query('BEGIN')
      await client.query(sqlDropTrigger(table))
      await client.query(sqlDropFunction(table))
      await client.query(sqlDropIndex(table))
      await client.query(sqlDropTsvColumn(table))
      await client.query('COMMIT')
    } catch (err) {
      await client.query('ROLLBACK')
      throw err
    } finally {
      client.release()
    }
  }

  async reindex(table: string, columns: ColumnMapping[], language = 'simple'): Promise<void> {
    await this.pool.query(sqlReindex(table, columns, language))
    await this.cache.invalidate(table)
  }

  async search<T extends Record<string, unknown> = Record<string, unknown>>(
    table: string,
    columns: ColumnMapping[],
    query: string,
    language = 'simple',
    options?: SearchOptions,
  ): Promise<SearchResult<T>> {
    const opts = resolveOptions(options)

    const cacheKey = `fts:${table}:${query}:${language}:${opts.limit}:${opts.offset}`

    return this.cache.wrap<SearchResult<T>>(
      cacheKey,
      () => this.executeSearch<T>(table, columns, query, language, opts),
      opts.cacheTTL,
    )
  }

  private async executeSearch<T extends Record<string, unknown>>(
    table: string,
    columns: ColumnMapping[],
    query: string,
    language: string,
    opts: Required<SearchOptions>,
  ): Promise<SearchResult<T>> {
    const like = `%${this.escapeLike(query)}%`

    const { query: searchSql, countQuery: countSql } = sqlSearch(
      table,
      columns,
      opts,
      language,
    )

    const params = [query, like, opts.offset, opts.limit]
    const countParams = [query, like]

    try {
      const [result, countResult] = await Promise.all([
        this.pool.query(searchSql, params),
        this.pool.query(countSql, countParams),
      ])

      const total = parseInt(countResult.rows[0].count, 10) || 0

      const hits: SearchHit<T>[] = result.rows.map((row: Record<string, unknown>) => {
        const rank = typeof (row as any).rank === 'number' ? (row as any).rank : 0
        const { rank: _r, ...source } = row as any
        return { rank, source: source as T }
      })

      return { total, hits }
    } catch (err) {
      console.error('[Ftssearch] Search error:', err)
      return { total: 0, hits: [] }
    }
  }

  getCacheStats(): CacheStats {
    return this.cache.getStats()
  }

  async invalidateCache(pattern?: string): Promise<void> {
    await this.cache.invalidate(pattern)
  }
}

export { Ftssearch as default, SearchCache }
export type { ColumnMapping, IndexConfig, SearchOptions, SearchResult, SearchHit, Weight, CacheStats, CacheOptions, FtssearchOptions } from './types.js'
export type { FtssearchConfigFile, ServerSection, CacheSection } from './config.js'
export { resolveConfigPath, loadConfigFile, getCacheOptions, SEARCH_PATHS } from './config.js'
export { WEIGHT_VALUES } from './types.js'
