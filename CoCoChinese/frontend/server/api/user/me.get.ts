export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)
  if (!auth) return { success: false, user: null }
  const { queryPg } = await import('../../utils/pg')
  const result = await queryPg('SELECT id, slug, username, role, avatar_url, display_name, email FROM users WHERE id = $1', [auth.userId])
  const user = result.rows[0] || auth
  return { success: true, user: { userId: user.id, username: user.username, role: user.role, avatar_url: user.avatar_url, slug: user.slug, display_name: user.display_name } }
})
