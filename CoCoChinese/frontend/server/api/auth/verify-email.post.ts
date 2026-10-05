export default defineEventHandler(async (event) => {
  const { token } = await readBody(event)
  if (!token) throw createError({ statusCode: 400, message: '缺少验证令牌' })

  const { queryPg } = await import('../../utils/pg')
  const verification = await queryPg(
    'SELECT * FROM email_verifications WHERE token = $1 AND verified_at IS NULL AND expires_at > NOW()',
    [token]
  )
  if (!verification.rows.length) {
    throw createError({ statusCode: 400, message: '验证链接无效或已过期' })
  }

  await queryPg(
    'UPDATE email_verifications SET verified_at = NOW() WHERE id = $1',
    [verification.rows[0].id]
  )
  await queryPg(
    'UPDATE users SET email = $1 WHERE id = $2',
    [verification.rows[0].email, verification.rows[0].user_id]
  )

  return { success: true, message: '邮箱已验证' }
})
