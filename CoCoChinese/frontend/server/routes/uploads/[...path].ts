import { readFile } from 'fs/promises'
import { join, extname } from 'path'

const MIME: Record<string, string> = {
  jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png',
  gif: 'image/gif', webp: 'image/webp', svg: 'image/svg+xml',
  mp4: 'video/mp4', webm: 'video/webm',
}

export default defineEventHandler(async (event) => {
  const pathname = getRequestURL(event).pathname
  const filePath = join(process.cwd(), 'public', pathname.slice(1))

  try {
    const data = await readFile(filePath)
    const ext = extname(pathname).slice(1).toLowerCase()
    if (MIME[ext]) setResponseHeader(event, 'Content-Type', MIME[ext])
    setResponseHeader(event, 'Cache-Control', 'public, max-age=31536000, immutable')
    return data
  } catch {
    // Fallback to default avatar for missing avatars
    if (pathname.startsWith('/uploads/avatars/')) {
      try {
        const defaultPath = join(process.cwd(), 'assets', 'authorImg.webp')
        const data = await readFile(defaultPath)
        setResponseHeader(event, 'Content-Type', 'image/webp')
        return data
      } catch {}
    }
    throw createError({ statusCode: 404, message: 'File not found' })
  }
})
