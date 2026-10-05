export default defineEventHandler(async (event) => {
  const id = parseInt(getRouterParam(event, 'id')!)
  const { queryPg } = await import('../../../utils/pg')

  const playlist = await queryPg('SELECT * FROM playlists WHERE id = $1', [id])
  if (!playlist.rows.length) throw createError({ statusCode: 404, message: '播放列表不存在' })

  const videos = await queryPg(`
    SELECT pv.*, v.*
    FROM playlist_videos pv
    JOIN videos v ON v.id = pv.video_id
    WHERE pv.playlist_id = $1
    ORDER BY pv."order" ASC
  `, [id])

  return { playlist: playlist.rows[0], videos: videos.rows }
})
