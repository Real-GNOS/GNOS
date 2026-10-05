export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const id = parseInt(getRouterParam(event, 'id')!)
  const { videoId } = await readBody(event)
  if (!videoId) throw createError({ statusCode: 400, message: '缺少视频ID' })

  const { queryPg } = await import('../../../utils/pg')

  const playlist = await queryPg('SELECT * FROM playlists WHERE id = $1 AND user_id = $2', [id, auth.userId])
  if (!playlist.rows.length) throw createError({ statusCode: 403, message: '无权操作' })

  const maxOrder = await queryPg('SELECT COALESCE(MAX("order"), -1) + 1 as next FROM playlist_videos WHERE playlist_id = $1', [id])
  const nextOrder = parseInt(maxOrder.rows[0].next)

  await queryPg(
    'INSERT INTO playlist_videos (playlist_id, video_id, "order") VALUES ($1, $2, $3) ON CONFLICT DO NOTHING',
    [id, videoId, nextOrder]
  )

  return { success: true }
})
