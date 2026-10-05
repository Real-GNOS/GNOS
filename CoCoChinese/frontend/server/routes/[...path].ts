import { readFile } from 'fs/promises'
import { join, extname } from 'path'

const MIME: Record<string, string> = {
  jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png',
  gif: 'image/gif', webp: 'image/webp', svg: 'image/svg+xml',
  mp4: 'video/mp4', webm: 'video/webm',
  js: 'application/javascript', css: 'text/css',
  json: 'application/json', pdf: 'application/pdf',
  ico: 'image/x-icon', mp3: 'audio/mpeg', wav: 'audio/wav',
}

export default defineEventHandler(async (event) => {
  let pathname = getRequestURL(event).pathname
  const filePath = join(process.cwd(), 'public', pathname)
  try {
    const data = await readFile(filePath)
    const ext = extname(pathname).slice(1).toLowerCase()
    if (MIME[ext]) setResponseHeader(event, 'Content-Type', MIME[ext])
    setResponseHeader(event, 'Cache-Control', 'public, max-age=31536000, immutable')
    return data
  } catch {
    throw createError({ statusCode: 404, message: `Page not found: ${pathname}` })
  }
})
