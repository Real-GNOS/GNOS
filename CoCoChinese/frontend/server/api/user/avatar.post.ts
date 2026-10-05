import { writeFile, mkdir } from 'fs/promises'
import { join } from 'path'

export default defineEventHandler(async (event) => {
  try {
    const auth = getAuthFromEvent(event)
    if (!auth) throw createError({ statusCode: 401, message: '请先登录' })
    const body = await readMultipartFormData(event)
    if (!body || !body.length) throw createError({ statusCode: 400, message: '请选择文件' })
    const file = body.find((p: any) => p.name === 'avatar')
    if (!file || !file.data) throw createError({ statusCode: 400, message: '请选择文件' })
    const ext = file.filename?.split('.').pop() || 'png'
    const filename = `avatar_${auth.userId}_${Date.now()}.${ext}`
    const publicDir = join(process.cwd(), 'public', 'uploads', 'avatars')
    await mkdir(publicDir, { recursive: true })
    await writeFile(join(publicDir, filename), file.data)
    const avatarUrl = `/uploads/avatars/${filename}`
    await queryPg('UPDATE users SET avatar_url = $1, updated_at = CURRENT_TIMESTAMP WHERE id = $2', [avatarUrl, parseInt(auth.userId)])

    const token = signToken({ userId: auth.userId, slug: auth.slug, username: auth.username, role: auth.role, avatar_url: avatarUrl })
    setCookie(event, 'cocokalo_user', JSON.stringify({
      userId: auth.userId, slug: auth.slug, username: auth.username, role: auth.role, avatar_url: avatarUrl, token
    }), { httpOnly: true, sameSite: 'strict', maxAge: 604800, path: '/' })
    setCookie(event, 'cocokalo_token', token, { httpOnly: true, sameSite: 'strict', maxAge: 604800, path: '/' })

    return { success: true, avatar_url: avatarUrl }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Avatar upload error:', err)
    throw createError({ statusCode: 500, message: '上传失败' })
  }
})
