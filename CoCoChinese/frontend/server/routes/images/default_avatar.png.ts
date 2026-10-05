import { readFile } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)

  if (auth) {
    try {
      const result = await queryPg('SELECT avatar_url FROM users WHERE id = $1', [parseInt(auth.userId)])
      const dbAvatar = result.rows[0]?.avatar_url
      if (dbAvatar && !dbAvatar.includes('default_avatar.png') && !dbAvatar.includes('authorImg.webp')) {
        const filePath = path.resolve('public', dbAvatar.replace(/^\//, ''))
        try {
          const data = await readFile(filePath)
          const ext = path.extname(filePath).toLowerCase()
          const mime: Record<string, string> = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp' }
          setHeader(event, 'Content-Type', mime[ext] || 'image/png')
          setHeader(event, 'Cache-Control', 'no-cache')
          return data
        } catch {}
      }
    } catch {}
  }

  const defaultPath = path.resolve('assets/default_avatar.png')
  try {
    const data = await readFile(defaultPath)
    setHeader(event, 'Content-Type', 'image/png')
    setHeader(event, 'Cache-Control', 'public, max-age=3600')
    return data
  } catch {
    throw createError({ statusCode: 404, message: 'Image not found' })
  }
})
