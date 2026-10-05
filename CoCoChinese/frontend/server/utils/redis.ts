import Redis from 'ioredis'

let redisInstance: Redis | null = null
let redisInitializing = false
let redisInitPromise: Promise<Redis | null> | null = null

const REDIS_URL = process.env.REDIS_URL || 'redis://localhost:6379'

async function initRedis(): Promise<Redis | null> {
  if (redisInstance) return redisInstance
  if (redisInitializing && redisInitPromise) return redisInitPromise
  redisInitializing = true
  redisInitPromise = new Promise((resolve) => {
    try {
      const r = new Redis(REDIS_URL, {
        maxRetriesPerRequest: 3,
        connectTimeout: 3000,
        retryStrategy(times) {
          if (times > 1) return null
          return 100
        },
        lazyConnect: true,
      })
      r.on('error', () => { redisInstance = null })
      redisInstance = r
      resolve(r)
    } catch {
      resolve(null)
    }
  })
  return redisInitPromise
}

const DEFAULT_TTL = 60

export function cacheKey(...parts: string[]) {
  return `cocokalo:${parts.join(':')}`
}

export async function cacheGet<T>(key: string): Promise<T | null> {
  try {
    const r = await initRedis()
    if (!r) return null
    const data = await r.get(key)
    return data ? JSON.parse(data) : null
  } catch {
    return null
  }
}

export async function cacheSet(key: string, data: unknown, ttl = DEFAULT_TTL) {
  try {
    const r = await initRedis()
    if (!r) return
    await r.setex(key, ttl, JSON.stringify(data))
  } catch {
    // silently fail
  }
}

export async function cacheDel(key: string) {
  try {
    const r = await initRedis()
    if (!r) return
    await r.del(key)
  } catch {
    // silently fail
  }
}

export async function cacheDelPattern(pattern: string) {
  try {
    const r = await initRedis()
    if (!r) return
    const keys = await r.keys(pattern)
    if (keys.length) await r.del(...keys)
  } catch {
    // silently fail
  }
}

export async function cacheWrap<T>(
  key: string,
  fn: () => Promise<T>,
  ttl = DEFAULT_TTL
): Promise<T> {
  const cached = await cacheGet<T>(key)
  if (cached !== null) return cached
  const data = await fn()
  await cacheSet(key, data, ttl)
  return data
}

export default redisInstance
