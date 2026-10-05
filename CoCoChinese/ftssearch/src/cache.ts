import type { CacheOptions, CacheStats } from './types.js'

interface MemoryEntry {
  data: unknown
  expiresAt: number
}

type RedisClient = {
  get(key: string): Promise<string | null>
  setex(key: string, seconds: number, value: string): Promise<unknown>
  del(...keys: string[]): Promise<unknown>
  keys(pattern: string): Promise<string[]>
  on(event: string, handler: (...args: unknown[]) => void): void
}

export class SearchCache {
  private memory: Map<string, MemoryEntry>
  private pending: Map<string, Promise<unknown>>
  private redis: RedisClient | null = null
  private redisConnected = false
  private redisInitAttempted = false

  private opts: Required<CacheOptions>

  public stats: CacheStats = {
    hits: 0,
    misses: 0,
    sets: 0,
    evictions: 0,
    memorySize: 0,
    pendingDedup: 0,
  }

  constructor(options?: CacheOptions) {
    this.opts = {
      enabled: true,
      ttl: 60,
      redisUrl: '',
      maxEntries: 1000,
      ...options,
    }
    this.memory = new Map()
    this.pending = new Map()

    if (this.opts.redisUrl) {
      this.tryInitRedis()
    }
  }

  private async tryInitRedis(): Promise<void> {
    if (this.redisInitAttempted) return
    this.redisInitAttempted = true
    try {
      const mod = await import('ioredis')
      const Redis = mod.default as unknown as new (url: string, opts: Record<string, unknown>) => RedisClient
      const r = new Redis(this.opts.redisUrl, {
        maxRetriesPerRequest: 2,
        connectTimeout: 2000,
        lazyConnect: true,
        retryStrategy: () => null,
      })
      r.on('error', () => {
        this.redisConnected = false
      })
      this.redis = r
      this.redisConnected = true
    } catch {
      this.redis = null
      this.redisConnected = false
    }
  }

  private memoryGet(key: string): unknown | null {
    const entry = this.memory.get(key)
    if (!entry) return null
    if (Date.now() > entry.expiresAt) {
      this.memory.delete(key)
      this.stats.memorySize = this.memory.size
      return null
    }
    this.memory.delete(key)
    this.memory.set(key, entry)
    return entry.data
  }

  private memorySet(key: string, data: unknown, ttl: number): void {
    if (this.memory.size >= this.opts.maxEntries) {
      const oldest = this.memory.keys().next().value
      if (oldest !== undefined) {
        this.memory.delete(oldest)
        this.stats.evictions++
      }
    }
    this.memory.set(key, {
      data,
      expiresAt: Date.now() + ttl * 1000,
    })
    this.stats.memorySize = this.memory.size
  }

  async get<T = unknown>(key: string): Promise<T | null> {
    if (!this.opts.enabled) return null

    const mem = this.memoryGet(key)
    if (mem !== null) {
      this.stats.hits++
      return mem as T
    }

    if (this.redisConnected && this.redis) {
      try {
        const raw = await this.redis.get(key)
        if (raw !== null) {
          const data = JSON.parse(raw) as T
          this.memorySet(key, data, this.opts.ttl)
          this.stats.hits++
          return data
        }
      } catch {
        this.redisConnected = false
      }
    }

    this.stats.misses++
    return null
  }

  async set(key: string, data: unknown, ttl?: number): Promise<void> {
    if (!this.opts.enabled) return
    const t = ttl ?? this.opts.ttl

    this.memorySet(key, data, t)

    if (this.redisConnected && this.redis) {
      try {
        await this.redis.setex(key, t, JSON.stringify(data))
      } catch {
        this.redisConnected = false
      }
    }

    this.stats.sets++
  }

  async wrap<T>(key: string, fn: () => Promise<T>, ttl?: number): Promise<T> {
    if (!this.opts.enabled) return fn()

    const existingPending = this.pending.get(key)
    if (existingPending) {
      this.stats.pendingDedup++
      return existingPending as Promise<T>
    }

    const cached = await this.get<T>(key)
    if (cached !== null) return cached

    const promise = fn()
      .then(async (data) => {
        await this.set(key, data, ttl)
        return data
      })
      .catch((err: unknown) => {
        throw err
      })
      .finally(() => {
        this.pending.delete(key)
      })

    this.pending.set(key, promise)
    return promise
  }

  async invalidate(pattern?: string): Promise<void> {
    if (pattern) {
      for (const key of this.memory.keys()) {
        if (key.includes(pattern)) this.memory.delete(key)
      }
      this.stats.memorySize = this.memory.size
      if (this.redisConnected && this.redis) {
        try {
          const keys = await this.redis.keys(`*${pattern}*`)
          if (keys.length) await this.redis.del(...keys)
        } catch {
          /* skip */
        }
      }
    } else {
      this.memory.clear()
      this.stats.memorySize = 0
      if (this.redisConnected && this.redis) {
        try {
          const keys = await this.redis.keys('fts:*')
          if (keys.length) await this.redis.del(...keys)
        } catch {
          /* skip */
        }
      }
    }
  }

  getStats(): CacheStats {
    return { ...this.stats, memorySize: this.memory.size }
  }
}
