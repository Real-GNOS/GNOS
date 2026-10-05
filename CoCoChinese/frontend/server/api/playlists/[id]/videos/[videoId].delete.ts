export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const id = parseInt(getRouterParam(event, 'id')!)
  const videoId = parseInt(getRouterParam(event, 'videoId')!)

  const { queryPg } = await import('../../../../utils/pg')
  const playlist = await queryPg('SELECT * FROM playlists WHERE id = $1 AND user_id = $2', [id, auth.userId])
  if (!playlist.rows.length) throw createError({ statusCode: 403, message: '无权操作' })

  await queryPg('DELETE FROM playlist_videos WHERE playlist_id = $1 AND video_id = $2', [id, videoId])

  return { success: true }
})
