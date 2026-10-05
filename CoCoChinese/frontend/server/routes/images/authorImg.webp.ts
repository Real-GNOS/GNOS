import { readFile } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)

  if (auth) {
    try {
      const result = await queryPg('SELECT avatar_url FROM users WHERE id = $1', [parseInt(auth.userId)])
      const avatarUrl = result.rows[0]?.avatar_url
      if (avatarUrl && !avatarUrl.includes('authorImg.webp') && !avatarUrl.startsWith('/images/')) {
        const filePath = path.resolve('public', avatarUrl.replace(/^\//, ''))
        try {
          const data = await readFile(filePath)
          const ext = path.extname(filePath).toLowerCase()
          const mime: Record<string, string> = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp' }
          setHeader(event, 'Content-Type', mime[ext] || 'image/png')
          return data
        } catch {}
      }
    } catch {}
  }

  const defaultPath = path.resolve('assets/authorImg.webp')
  try {
    const data = await readFile(defaultPath)
    setHeader(event, 'Content-Type', 'image/webp')
    return data
  } catch {
    throw createError({ statusCode: 404, message: 'Image not found' })
  }
})
