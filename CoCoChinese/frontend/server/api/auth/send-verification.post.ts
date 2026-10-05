import { randomBytes } from 'crypto'

export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)
  if (!auth) throw createError({ statusCode: 401, message: '请先登录' })

  const { email } = await readBody(event)
  if (!email) throw createError({ statusCode: 400, message: '请输入邮箱' })

  const { queryPg } = await import('../../utils/pg')

  // Invalidate old tokens
  await queryPg(
    'UPDATE email_verifications SET expires_at = NOW() WHERE user_id = $1 AND verified_at IS NULL',
    [auth.userId]
  )

  const token = randomBytes(32).toString('hex')
  const expiresAt = new Date(Date.now() + 86400000) // 24h

  await queryPg(
    'INSERT INTO email_verifications (user_id, email, token, expires_at) VALUES ($1, $2, $3, $4)',
    [auth.userId, email, token, expiresAt]
  )

  const siteUrl = process.env.SITE_URL || 'http://localhost:3000'
  const verifyLink = `${siteUrl}/verify-email?token=${token}`

  // Send email
  try {
    const nodemailer = await import('nodemailer').catch(() => null)
    if (nodemailer) {
      const transporter = nodemailer.default.createTransport({
        host: process.env.SMTP_HOST || 'smtp.gmail.com',
        port: parseInt(process.env.SMTP_PORT || '587'),
        secure: false,
        auth: { user: process.env.SMTP_USER, pass: process.env.SMTP_PASS },
      })
      await transporter.sendMail({
        from: process.env.SMTP_FROM || 'noreply@cocokalo.com',
        to: email,
        subject: 'Cocokalo - 邮箱验证',
        html: `<p>点击以下链接验证邮箱：</p><a href="${verifyLink}">${verifyLink}</a><p>链接有效期24小时</p>`,
      })
    }
  } catch (e) {
    console.error('Failed to send verification email:', e)
  }

  return { success: true, message: '验证邮件已发送' }
})
