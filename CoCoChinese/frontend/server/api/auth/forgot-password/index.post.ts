import { randomBytes } from 'crypto'

export default defineEventHandler(async (event) => {
  const { email } = await readBody(event)
  if (!email) throw createError({ statusCode: 400, message: '请输入邮箱' })

  const { queryPg } = await import('../../../utils/pg')
  const user = await queryPg('SELECT id FROM users WHERE email = $1', [email])
  if (!user.rows.length) {
    return { success: true, message: '如果该邮箱已注册，重置链接已发送' }
  }

  const token = randomBytes(32).toString('hex')
  const expiresAt = new Date(Date.now() + 3600000)

  await queryPg(
    'INSERT INTO password_resets (user_id, token, expires_at) VALUES ($1, $2, $3)',
    [user.rows[0].id, token, expiresAt]
  )

  const siteUrl = process.env.SITE_URL || 'http://localhost:3000'
  const resetLink = `${siteUrl}/password-reset?token=${token}`

  // Send email via configured provider
  const emailProvider = process.env.EMAIL_PROVIDER || 'smtp'
  try {
    if (emailProvider === 'smtp') {
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
          subject: 'Cocokalo - 密码重置',
          html: `<p>点击以下链接重置密码：</p><a href="${resetLink}">${resetLink}</a><p>链接有效期1小时</p>`,
        })
      }
    }
  } catch (e) {
    console.error('Failed to send email:', e)
  }

  return { success: true, message: '如果该邮箱已注册，重置链接已发送' }
})
