export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  const auth = getAuthFromEvent(event)

  const { queryPg } = await import('../../utils/pg')
  const result = await queryPg('SELECT * FROM videos WHERE slug = $1 AND (is_deleted = false OR is_deleted IS NULL)', [slug])

  if (!result.rows.length) {
    throw createError({ statusCode: 404, message: '视频不存在' })
  }

  const video = result.rows[0]

  // Increment view count
  await queryPg(
    "UPDATE videos SET watch_volue = (COALESCE(NULLIF(watch_volue, ''), '0')::BIGINT + 1)::VARCHAR WHERE id = $1",
    [video.id]
  )

  // Record watch history if logged in
  if (auth) {
    await queryPg(`
      INSERT INTO watch_history (user_id, video_id, progress, watched_at)
      VALUES ($1, $2, 0, NOW())
      ON CONFLICT ON CONSTRAINT watch_history_user_id_video_id_key
      DO UPDATE SET watched_at = NOW(), progress = 0
    `, [auth.userId, video.id]).catch(() => {})
  }

  return { video }
})
