import type { Pool } from 'pg'

export type Weight = 'A' | 'B' | 'C' | 'D'

export const WEIGHT_VALUES: Record<Weight, number> = { D: 0.1, C: 0.2, B: 0.4, A: 1.0 }

export interface ColumnMapping {
  column: string
  weight: Weight
  isArray?: boolean
}

export interface IndexConfig {
  table: string
  columns: ColumnMapping[]
  language?: string
}

export interface SearchOptions {
  limit?: number
  offset?: number
  weights?: [number, number, number, number]
  normalize?: number
  fallbackToLike?: boolean
  cacheTTL?: number
}

export interface SearchHit<T = Record<string, unknown>> {
  rank: number
  source: T
}

export interface SearchResult<T = Record<string, unknown>> {
  total: number
  hits: SearchHit<T>[]
}

export interface CacheStats {
  hits: number
  misses: number
  sets: number
  evictions: number
  memorySize: number
  pendingDedup: number
}

export interface CacheOptions {
  enabled?: boolean
  ttl?: number
  redisUrl?: string
  maxEntries?: number
}

export interface FtssearchConfig {
  connectionString?: string
  pool?: Pool
}

export interface FtssearchOptions {
  cache?: CacheOptions | false
}

export interface ServerConfig {
  port?: number
  host?: string
  table?: string
  columns?: ColumnMapping[]
  language?: string
  cors?: boolean
}
