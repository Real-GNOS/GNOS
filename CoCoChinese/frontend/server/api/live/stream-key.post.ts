import { randomBytes } from 'crypto'

export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { title } = await readBody(event)

  const streamKey = randomBytes(16).toString('hex')
  const { queryPg } = await import('../../utils/pg')

  const existing = await queryPg('SELECT id FROM streams WHERE user_id = $1 AND status = $2', [auth.userId, 'live'])
  if (existing.rows.length) {
    throw createError({ statusCode: 400, message: '已有进行中的直播' })
  }

  const result = await queryPg(
    'INSERT INTO streams (user_id, title, stream_key, status) VALUES ($1, $2, $3, $4) RETURNING *',
    [auth.userId, title || `${auth.username}的直播`, streamKey, 'ready']
  )

  return { stream: result.rows[0] }
})
