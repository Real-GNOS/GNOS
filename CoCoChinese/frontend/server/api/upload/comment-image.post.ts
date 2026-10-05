import { writeFile, mkdir } from 'fs/promises'
import path from 'path'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readMultipartFormData(event)
    if (!body) throw createError({ statusCode: 400, message: '没有文件' })
    const file = body.find(f => f.name === 'image')
    if (!file || !file.data) throw createError({ statusCode: 400, message: '请选择图片' })
    const ext = path.extname(file.filename || '.png') || '.png'
    const name = `comment_${auth.userId}_${Date.now()}_${Math.random().toString(36).slice(2, 6)}${ext}`
    const dir = path.resolve('public/uploads/comments')
    await mkdir(dir, { recursive: true })
    await writeFile(path.join(dir, name), file.data)
    return { success: true, url: `/uploads/comments/${name}` }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Comment image upload error:', err)
    throw createError({ statusCode: 500, message: '上传失败' })
  }
})
