import { randomBytes } from 'crypto'

export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { targetType, targetId } = await readBody(event)

  if (!['video', 'post', 'playlist'].includes(targetType)) {
    throw createError({ statusCode: 400, message: '无效的分享类型' })
  }

  const { queryPg } = await import('../../utils/pg')
  const code = randomBytes(4).toString('hex')

  await queryPg(
    'INSERT INTO share_links (user_id, target_type, target_id, code) VALUES ($1, $2, $3, $4)',
    [auth.userId, targetType, targetId, code]
  )

  const siteUrl = process.env.SITE_URL || 'http://localhost:3000'
  return { url: `${siteUrl}/s/${code}`, code }
})
