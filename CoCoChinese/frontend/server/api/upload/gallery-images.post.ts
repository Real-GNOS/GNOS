import { writeFile, mkdir } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readMultipartFormData(event)
    if (!body) throw createError({ statusCode: 400, message: '没有数据' })
    const files = body.filter(f => f.name === 'files')
    if (!files.length) throw createError({ statusCode: 400, message: '请选择图片' })
    const allowed = ['.jpg', '.jpeg', '.png', '.gif', '.webp']
    const urls: string[] = []
    const dir = path.resolve('public/images')
    await mkdir(dir, { recursive: true })
    for (const file of files) {
      if (!file.data || !file.data.length) continue
      const ext = path.extname(file.filename || '.webp') || '.webp'
      if (!allowed.includes(ext.toLowerCase())) continue
      if (file.data.length > 10 * 1024 * 1024) continue
      const name = `${auth.username}_${Date.now()}_${Math.random().toString(36).slice(2, 8)}${ext}`
      await writeFile(path.join(dir, name), file.data)
      urls.push(`/images/${name}`)
    }
    if (!urls.length) throw createError({ statusCode: 400, message: '没有上传成功的图片' })
    return { success: true, urls }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Gallery upload error:', err)
    throw createError({ statusCode: 500, message: '上传失败' })
  }
})
