export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { id } = await readBody(event)
  const { queryPg } = await import('../../utils/pg')

  if (id) {
    await queryPg(
      'UPDATE notifications SET is_read = true WHERE id = $1 AND user_id = $2',
      [id, auth.userId]
    )
  } else {
    await queryPg(
      'UPDATE notifications SET is_read = true WHERE user_id = $1',
      [auth.userId]
    )
  }

  return { success: true }
})
