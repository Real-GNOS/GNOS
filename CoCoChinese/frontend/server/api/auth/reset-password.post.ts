import bcrypt from 'bcryptjs'

export default defineEventHandler(async (event) => {
  const { token, password } = await readBody(event)
  if (!token || !password) throw createError({ statusCode: 400, message: '参数不完整' })
  if (password.length < 8) throw createError({ statusCode: 400, message: '密码至少8位' })

  const { queryPg } = await import('../../utils/pg')

  const reset = await queryPg(
    'SELECT * FROM password_resets WHERE token = $1 AND used_at IS NULL AND expires_at > NOW()',
    [token]
  )
  if (!reset.rows.length) {
    throw createError({ statusCode: 400, message: '重置链接无效或已过期' })
  }

  const salt = await bcrypt.genSalt(10)
  const hash = await bcrypt.hash(password, salt)

  await queryPg('UPDATE users SET password = $1 WHERE id = $2', [hash, reset.rows[0].user_id])
  await queryPg('UPDATE password_resets SET used_at = NOW() WHERE id = $1', [reset.rows[0].id])

  return { success: true, message: '密码已重置' }
})
