export default defineEventHandler(async (event) => {
  const bcrypt = (await import('bcryptjs')).default
  try {
    const body = await readBody(event)
    if (!body || !body.username || body.username.length < 4) {
      throw createError({ statusCode: 400, message: '用户名至少4个字符' })
    }
    if (!body || !body.password || body.password.length < 8) {
      throw createError({ statusCode: 400, message: '密码至少8个字符' })
    }

    const existing = await findUserByUsername(body.username)
    if (existing) {
      throw createError({ statusCode: 409, message: '用户名已存在' })
    }

    const salt = await bcrypt.genSalt(10)
    const hash = await bcrypt.hash(body.password, salt)
    const userSlug = await uniqueSlug('users')
    const newUser = await queryPg(
      'INSERT INTO users (slug, username, password, role, display_name) VALUES ($1, $2, $3, $4, $5) RETURNING id',
      [userSlug, body.username, hash, 'user', '']
    )
    await queryPg('INSERT INTO user_profiles (user_id) VALUES ($1)', [newUser.rows[0].id])

    const avatarUrl = '/images/authorImg.webp'
    const token = signToken({ userId: String(newUser.rows[0].id), slug: userSlug, username: body.username, role: 'user', avatar_url: avatarUrl, display_name: '' })
    setCookie(event, 'cocokalo_user', JSON.stringify({
      userId: String(newUser.rows[0].id),
      slug: userSlug,
      username: body.username,
      role: 'user',
      avatar_url: avatarUrl,
      display_name: '',
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

    return { success: true, message: '注册成功', token }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Register error:', err)
    throw createError({ statusCode: 500, message: '注册失败' })
  }
})
