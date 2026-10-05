export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readBody(event)
    if (!body) throw createError({ statusCode: 400, message: '参数错误' })
    await updateUserProfile(parseInt(auth.userId), body)

    const displayName = body.display_name?.trim() ?? ''
    const avatarUrl = auth.avatar_url || '/images/authorImg.webp'
    const token = signToken({ userId: auth.userId, slug: auth.slug, username: auth.username, role: auth.role, avatar_url: avatarUrl, display_name: displayName })

    setCookie(event, 'cocokalo_user', JSON.stringify({
      userId: auth.userId, slug: auth.slug, username: auth.username,
      role: auth.role, avatar_url: avatarUrl, display_name: displayName, token
    }), { httpOnly: true, sameSite: 'strict', maxAge: 604800, path: '/' })
    setCookie(event, 'cocokalo_token', token, { httpOnly: true, sameSite: 'strict', maxAge: 604800, path: '/' })

    return { success: true, message: '更新成功', display_name: displayName }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Profile update error:', err)
    throw createError({ statusCode: 500, message: '更新失败' })
  }
})
