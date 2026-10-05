export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { endpoint, keys } = await readBody(event)
  const { queryPg } = await import('../utils/pg')

  await queryPg(
    `INSERT INTO push_subscriptions (user_id, endpoint, p256dh, auth, user_agent)
     VALUES ($1, $2, $3, $4, $5)
     ON CONFLICT DO NOTHING`,
    [auth.userId, endpoint, keys.p256dh, keys.auth, getHeader(event, 'user-agent') || '']
  )

  return { success: true }
})
