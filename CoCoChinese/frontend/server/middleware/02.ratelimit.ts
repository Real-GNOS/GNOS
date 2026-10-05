const requestCounts = new Map<string, { count: number; resetAt: number }>()

export default defineEventHandler(async (event) => {
  const path = getRequestURL(event).pathname

  if (path.startsWith('/_nuxt/') || path.startsWith('/images/') || path.startsWith('/setup') || path.startsWith('/api/setup') || path === '/api/user/me') {
    return
  }

  const windowMs = parseInt(process.env.RATE_LIMIT_WINDOW || '60') * 1000
  const maxRequests = parseInt(process.env.RATE_LIMIT_MAX || '100')

  const ip = getRequestIP(event, { xForwardedFor: true }) || 'unknown'
  const segments = path.split('/')
  const key = `${ip}:${segments.slice(1, 3).join('/')}`
  const now = Date.now()

  let record = requestCounts.get(key)

  if (!record || now > record.resetAt) {
    record = { count: 0, resetAt: now + windowMs }
    requestCounts.set(key, record)
  }

  record.count++

  if (record.count > maxRequests) {
    throw createError({
      statusCode: 429,
      statusMessage: 'Too Many Requests',
      message: '请求过于频繁，请稍后重试',
    })
  }

  // Clean up old entries periodically
  if (requestCounts.size > 10000) {
    for (const [k, v] of requestCounts) {
      if (now > v.resetAt) requestCounts.delete(k)
    }
  }
})
