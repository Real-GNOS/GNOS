export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })

  const tokenCookie = getCookie(event, 'cocokalo_token')
  const { verifyToken } = await import('~/server/utils/jwt')
  if (!tokenCookie || !verifyToken(tokenCookie)) {
    throw createError({ statusCode: 403, message: '请登录后访问' })
  }

  const fs = await import('fs/promises')
  const path = await import('path')
  const videoJsonPath = path.default.resolve(`storage/fmp4/${slug}/video.json`)

  try {
    const data = await fs.default.readFile(videoJsonPath, 'utf-8')
    setHeader(event, 'Content-Type', 'application/json')
    setHeader(event, 'Cache-Control', 'public, max-age=86400')
    return data
  } catch {
    throw createError({ statusCode: 404, message: '视频索引不存在' })
  }
})
