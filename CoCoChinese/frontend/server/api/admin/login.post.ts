export default defineEventHandler(async (event) => {
  try {
    const { username, password } = await readBody(event)
    const user = await findUserByUsername(username)
    if (!user || user.role !== 'admin') {
      throw createError({ statusCode: 401, message: '无效的管理员凭证' })
    }
    const isMatch = await comparePassword(password, user.password)
    if (!isMatch) {
      throw createError({ statusCode: 401, message: '无效的管理员凭证' })
    }

    const avatarUrl = user.avatar_url || '/images/authorImg.webp'
    const slug = user.slug || String(user.id)
    const displayName = user.display_name || ''
    const token = signToken({ userId: String(user.id), slug, username: user.username, role: user.role, avatar_url: avatarUrl, display_name: displayName })
    setCookie(event, 'cocokalo_user', JSON.stringify({
      userId: String(user.id),
      slug,
      username: user.username,
      role: user.role,
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

    return { success: true, redirect: '/admin/dashboard', token }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Admin login error:', err)
    throw createError({ statusCode: 500, message: '登录失败' })
  }
})
