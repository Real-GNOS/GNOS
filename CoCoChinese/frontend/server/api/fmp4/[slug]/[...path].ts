import { readFile } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  const segPath = getRouterParam(event, 'path')
  if (!slug || !segPath) throw createError({ statusCode: 400, message: '缺少参数' })

  const tokenCookie = getCookie(event, 'cocokalo_token')
  const { verifyToken } = await import('~/server/utils/jwt')
  if (!tokenCookie || !verifyToken(tokenCookie)) {
    throw createError({ statusCode: 403, message: '请登录后访问' })
  }

  const query = getQuery(event)
  const token = query._vt as string
  const { verifyVideoToken } = await import('~/server/utils/video-token')
  const payload = verifyVideoToken(token || '')
  if (!payload || payload.videoId !== slug) {
    throw createError({ statusCode: 403, message: '视频链接已过期，请刷新页面重试' })
  }

  if (segPath.includes('..')) {
    throw createError({ statusCode: 400, message: '无效路径' })
  }

  const fullPath = path.resolve('storage/fmp4', slug, segPath)

  try {
    const data = await readFile(fullPath)
    setHeader(event, 'Content-Type', 'video/mp4')
    setHeader(event, 'Cache-Control', 'public, max-age=86400')
    setHeader(event, 'X-Content-Type-Options', 'nosniff')
    setHeader(event, 'Access-Control-Allow-Origin', '*')
    return data
  } catch {
    throw createError({ statusCode: 404, message: '分片文件未找到' })
  }
})
