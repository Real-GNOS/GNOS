export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { queryPg } = await import('../../utils/pg')

  const sub = await queryPg(
    'SELECT * FROM subscriptions WHERE user_id = $1 AND status = $2',
    [auth.userId, 'active']
  )

  const history = await queryPg(
    'SELECT * FROM payments WHERE user_id = $1 ORDER BY created_at DESC LIMIT 20',
    [auth.userId]
  )

  return {
    subscription: sub.rows[0] || null,
    history: history.rows,
  }
})
