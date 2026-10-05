import { readFile } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  const filePath = getRouterParam(event, 'path')
  if (!filePath || filePath.includes('..')) {
    throw createError({ statusCode: 400, message: 'Invalid path' })
  }

  const tokenCookie = getCookie(event, 'cocokalo_token')
  const { verifyToken } = await import('~/server/utils/jwt')
  if (!tokenCookie || !verifyToken(tokenCookie)) {
    throw createError({ statusCode: 403, message: '请登录后访问视频' })
  }

  const query = getQuery(event)
  const token = query._vt as string
  const { verifyVideoToken } = await import('~/server/utils/video-token')
  const payload = verifyVideoToken(token || '')

  const pathParts = filePath.split('/')
  const videoIdentifier = pathParts.length > 1 ? pathParts[0] : path.parse(filePath).name.replace(/_\d+p$/, '')

  if (!payload || payload.videoId !== videoIdentifier) {
    throw createError({ statusCode: 403, message: '视频链接已过期，请刷新页面重试' })
  }

  const fullPath = path.resolve('storage/videos', filePath)
  try {
    const ext = path.extname(filePath).toLowerCase()
    const mime: Record<string, string> = {
      '.mp4': 'video/mp4',
      '.webm': 'video/webm',
      '.ogg': 'video/ogg',
      '.flv': 'video/x-flv',
      '.avi': 'video/x-msvideo',
      '.mov': 'video/quicktime',
      '.m4s': 'video/mp4',
      '.ts': 'video/mp2t',
      '.tsv': 'video/mp2t',
      '.tsa': 'audio/mp2t',
      '.m3u8': 'application/vnd.apple.mpegurl',
      '.m4v': 'video/mp4',
    }

    if (ext === '.m3u8' && payload) {
      const m3u8Content = await readFile(fullPath, 'utf-8')
      const baseUrl = filePath.substring(0, filePath.lastIndexOf('/'))
      const lines = m3u8Content.split('\n')
      const rewritten = lines.map(line => {
        const trimmed = line.trim()
        if (trimmed && !trimmed.startsWith('#') && !trimmed.startsWith('http')) {
          const sep = trimmed.includes('?') ? '&' : '?'
          return trimmed + sep + '_vt=' + encodeURIComponent(token)
        }
        if (trimmed.startsWith('#EXT-X-MEDIA:')) {
          return trimmed.replace(/URI="([^"]+)"/g, (_, uri) => {
            const sep = uri.includes('?') ? '&' : '?'
            return `URI="${uri}${sep}_vt=${encodeURIComponent(token)}"`
          })
        }
        return line
      }).join('\n')
      setHeader(event, 'Content-Type', mime['.m3u8'] || 'application/vnd.apple.mpegurl')
      setHeader(event, 'X-Content-Type-Options', 'nosniff')
      setHeader(event, 'Cache-Control', 'no-store, no-cache, must-revalidate')
      return rewritten
    }

    const data = await readFile(fullPath)
    setHeader(event, 'Content-Type', mime[ext] || 'application/octet-stream')

    if (ext === '.mp4') {
      setHeader(event, 'Accept-Ranges', 'bytes')
    }

    setHeader(event, 'Content-Disposition', 'inline')
    setHeader(event, 'X-Content-Type-Options', 'nosniff')
    setHeader(event, 'Cache-Control', 'no-store, no-cache, must-revalidate')

    const referer = getHeader(event, 'referer') || ''
    if (referer && !referer.includes('/player/') && !referer.includes('/admin/') && !payload?.canDownload) {
      setHeader(event, 'X-Robots-Tag', 'noindex, nofollow')
    }

    return data
  } catch {
    throw createError({ statusCode: 404, message: '视频文件未找到' })
  }
})
