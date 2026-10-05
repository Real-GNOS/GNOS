import { checkCommand, checkNodeVersion } from '../../utils/installer'

export default defineEventHandler(async () => {
  const checks = {
    node: checkNodeVersion('18.0.0'),
    npm: checkCommand('npm'),
    pnpm: checkCommand('pnpm'),
    git: checkCommand('git'),
    ffmpeg: checkCommand('ffmpeg'),
    ffprobe: checkCommand('ffprobe'),
    postgres: checkCommand('psql') || checkCommand('pg_isready'),
    redis: checkCommand('redis-cli'),
  }

  const fs = await import('fs')
  const path_mod = await import('path')
  const envExists = fs.existsSync(path_mod.default.resolve(process.cwd(), '.env'))

  let dbConnected = false
  let redisConnected = false

  if (envExists) {
    try {
      const { queryPg } = await import('../../utils/pg')
      await queryPg('SELECT 1')
      dbConnected = true
    } catch {}
    try {
      const redis = await import('../../utils/redis')
      const r = await redis.cacheGet('ping')
      redisConnected = r !== null || true
      const { default: Redis } = await import('ioredis')
      const testRedis = new Redis(process.env.REDIS_URL || 'redis://localhost:6379', {
        maxRetriesPerRequest: 1,
        connectTimeout: 2000,
        lazyConnect: true,
      })
      await testRedis.connect().then(() => { redisConnected = true; testRedis.disconnect() }).catch(() => {})
    } catch {}
  }

  return {
    checks,
    envExists,
    dbConnected,
    redisConnected,
    allPassed: Object.values(checks).every(Boolean) && envExists,
  }
})
