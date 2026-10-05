export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { queryPg } = await import('../../utils/pg')

  const notifications = await queryPg(
    'SELECT * FROM notifications WHERE user_id = $1 ORDER BY created_at DESC LIMIT 50',
    [auth.userId]
  )

  const unreadCount = await queryPg(
    'SELECT COUNT(*) FROM notifications WHERE user_id = $1 AND is_read = false',
    [auth.userId]
  )

  return {
    notifications: notifications.rows,
    unreadCount: parseInt(unreadCount.rows[0].count),
  }
})
