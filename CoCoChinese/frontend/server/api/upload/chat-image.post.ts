import { writeFile, mkdir } from 'fs/promises'
import path from 'path'

const ALLOWED_TYPES = ['image/jpeg', 'image/png', 'image/gif', 'image/webp']
const MAX_SIZE = 10 * 1024 * 1024

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readMultipartFormData(event)
    if (!body) throw createError({ statusCode: 400, message: '没有数据' })

    const fileField = body.find(f => f.name === 'file')
    if (!fileField || !fileField.data || !fileField.data.length) {
      throw createError({ statusCode: 400, message: '请选择图片' })
    }

    if (!ALLOWED_TYPES.includes(fileField.type)) {
      throw createError({ statusCode: 400, message: '不支持的图片格式，支持jpg/png/gif/webp' })
    }
    if (fileField.data.length > MAX_SIZE) {
      throw createError({ statusCode: 400, message: '图片大小不能超过10MB' })
    }

    const ext = path.extname(fileField.filename || '.png') || '.png'
    const slug = `chat_${auth.userId}_${Date.now()}${ext}`
    const chatDir = path.resolve('public/images/chat')

    await mkdir(chatDir, { recursive: true })
    await writeFile(path.join(chatDir, slug), fileField.data)

    return { success: true, url: `/images/chat/${slug}` }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Chat image upload error:', err)
    throw createError({ statusCode: 500, message: '图片上传失败' })
  }
})
