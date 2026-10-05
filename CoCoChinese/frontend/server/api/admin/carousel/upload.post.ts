import { writeFile, mkdir } from 'fs/promises'
import path from 'path'
import { requireAdmin } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  try {
    await requireAdmin(event)
    const body = await readMultipartFormData(event)
    if (!body) throw createError({ statusCode: 400, message: '没有文件' })
    const file = body.find(f => f.name === 'image')
    if (!file || !file.data) throw createError({ statusCode: 400, message: '请选择图片' })

    const allowed = ['.png', '.jpg', '.jpeg', '.webp', '.gif', '.bmp']
    const ext = (path.extname(file.filename || '') || '').toLowerCase()
    if (!allowed.includes(ext)) throw createError({ statusCode: 400, message: '仅支持图片格式（png/jpg/webp 等）' })

    const name = `carousel_${Date.now()}_${Math.random().toString(36).slice(2, 8)}${ext}`
    const dir = path.resolve('public/uploads/carousel')
    await mkdir(dir, { recursive: true })
    await writeFile(path.join(dir, name), file.data)
    return { success: true, url: `/uploads/carousel/${name}` }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Carousel upload error:', err)
    throw createError({ statusCode: 500, message: '上传失败' })
  }
})
