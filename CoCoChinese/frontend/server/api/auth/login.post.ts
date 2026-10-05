export default defineEventHandler(async (event) => {
  try {
    const body = await readBody(event)
    if (!body || !body.username || !body.password) {
      throw createError({ statusCode: 400, message: '请填写用户名和密码' })
    }
    const user = await findUserByUsername(body.username)
    if (!user) {
      throw createError({ statusCode: 401, message: '用户名或密码错误' })
    }
    const isMatch = await comparePassword(body.password, user.password)
    if (!isMatch) {
      throw createError({ statusCode: 401, message: '用户名或密码错误' })
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
    return { success: true, user: { id: user.id, slug, username: user.username, role: user.role, avatar_url: avatarUrl, display_name: displayName }, token }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Login error:', err)
    throw createError({ statusCode: 500, message: '登录失败' })
  }
})
