import { writeFile, mkdir } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readMultipartFormData(event)
    if (!body) throw createError({ statusCode: 400, message: '没有数据' })
    const fileField = body.find(f => f.name === 'file')
    if (!fileField || !fileField.data || !fileField.data.length) {
      throw createError({ statusCode: 400, message: '请选择图片' })
    }
    const ext = path.extname(fileField.filename || '.webp') || '.webp'
    const allowed = ['.jpg', '.jpeg', '.png', '.gif', '.webp']
    if (!allowed.includes(ext.toLowerCase())) {
      throw createError({ statusCode: 400, message: '不支持的图片格式' })
    }
    if (fileField.data.length > 10 * 1024 * 1024) {
      throw createError({ statusCode: 400, message: '图片大小不能超过10MB' })
    }
    const name = `${auth.username}_${Date.now()}_${Math.random().toString(36).slice(2, 8)}${ext}`
    const dir = path.resolve('public/images')
    await mkdir(dir, { recursive: true })
    await writeFile(path.join(dir, name), fileField.data)
    return { success: true, url: `/images/${name}` }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Article image upload error:', err)
    throw createError({ statusCode: 500, message: '上传失败' })
  }
})
