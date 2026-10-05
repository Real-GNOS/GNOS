export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { videoId, folderId } = await readBody(event)
  if (!videoId) throw createError({ statusCode: 400, message: '缺少视频ID' })

  const { queryPg } = await import('../../utils/pg')
  const existing = await queryPg(
    'SELECT id FROM favorites WHERE user_id = $1 AND video_id = $2',
    [auth.userId, videoId]
  )

  if (existing.rows.length) {
    await queryPg('DELETE FROM favorites WHERE id = $1', [existing.rows[0].id])
    return { favorited: false }
  } else {
    await queryPg(
      'INSERT INTO favorites (user_id, video_id, folder_id) VALUES ($1, $2, $3)',
      [auth.userId, videoId, folderId || null]
    )
    return { favorited: true }
  }
})
