export default defineEventHandler(async (event) => {
  const body = await readBody(event)
  try {
    const { queryPg, uniqueSlug } = await import('../../utils/pg')
    const bcrypt = await import('bcryptjs')

    const existing = await queryPg('SELECT id FROM users WHERE username = $1', ['admin'])
    if (existing.rows.length) {
      return { success: true, message: '管理员已存在' }
    }

    const slug = await uniqueSlug('users')
    const salt = await bcrypt.genSalt(10)
    const hash = await bcrypt.hash(body.password || 'admin123', salt)
    await queryPg(
      'INSERT INTO users (slug, username, password, role) VALUES ($1, $2, $3, $4)',
      [slug, body.username || 'admin', hash, 'admin']
    )
    return { success: true, message: '管理员账号已创建' }
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `创建管理员失败: ${e.message}` })
  }
})
