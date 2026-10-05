import { requireAuth } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const body = await readBody(event)
  
  const displayName = body.display_name?.trim() || ''
  if (displayName.length > 100) {
    throw createError({ statusCode: 400, message: '昵称不能超过100个字符' })
  }

  try {
    await queryPg('UPDATE users SET display_name = $1, updated_at = CURRENT_TIMESTAMP WHERE id = $2', [displayName, parseInt(auth.userId)])
    
    // 更新 token 和 cookie
    const avatarUrl = auth.avatar_url || '/images/authorImg.webp'
    const token = signToken({ userId: auth.userId, slug: auth.slug, username: auth.username, role: auth.role, avatar_url: avatarUrl, display_name: displayName })
    
    setCookie(event, 'cocokalo_user', JSON.stringify({
      userId: auth.userId,
      slug: auth.slug,
      username: auth.username,
      role: auth.role,
      avatar_url: avatarUrl,
      display_name: displayName,
      token
    }), {
      httpOnly: true,
      sameSite: 'strict',
      maxAge: 604800,
      path: '/'
    })
    setCookie(event, 'cocokalo_token', token, {
      httpOnly: true,
      sameSite: 'strict',
      maxAge: 604800,
      path: '/'
    })

    return { success: true, display_name: displayName }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Update display name error:', err)
    throw createError({ statusCode: 500, message: '更新失败' })
  }
})