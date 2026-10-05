export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { name, description, isPublic } = await readBody(event)
  if (!name) throw createError({ statusCode: 400, message: '请输入播放列表名称' })

  const { queryPg } = await import('../../utils/pg')
  const result = await queryPg(
    'INSERT INTO playlists (user_id, name, description, is_public) VALUES ($1, $2, $3, $4) RETURNING *',
    [auth.userId, name, description || '', isPublic !== false]
  )

  return { playlist: result.rows[0] }
})
