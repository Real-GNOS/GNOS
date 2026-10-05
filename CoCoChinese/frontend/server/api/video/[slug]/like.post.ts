export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  const auth = await requireAuth(event)

  const { queryPg } = await import('../../../utils/pg')
  const video = await queryPg('SELECT id, like_volue FROM videos WHERE slug = $1', [slug])
  if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

  const videoId = video.rows[0].id
  const existing = await queryPg(
    'SELECT id FROM video_likes WHERE user_id = $1 AND video_id = $2',
    [auth.userId, videoId]
  )

  if (existing.rows.length) {
    await queryPg('DELETE FROM video_likes WHERE id = $1', [existing.rows[0].id])
    const newCount = Math.max(0, (parseInt(video.rows[0].like_volue) || 1) - 1)
    await queryPg('UPDATE videos SET like_volue = $1 WHERE id = $2', [String(newCount), videoId])
    return { liked: false, count: newCount }
  } else {
    await queryPg(
      'INSERT INTO video_likes (user_id, video_id) VALUES ($1, $2)',
      [auth.userId, videoId]
    )
    const newCount = (parseInt(video.rows[0].like_volue) || 0) + 1
    await queryPg('UPDATE videos SET like_volue = $1 WHERE id = $2', [String(newCount), videoId])

    try {
      const videoInfo = await queryPg('SELECT author, title FROM videos WHERE slug = $1', [slug])
      if (videoInfo.rows.length) {
        const authorUser = await queryPg('SELECT id FROM users WHERE username = $1', [videoInfo.rows[0].author])
        if (authorUser.rows.length && parseInt(authorUser.rows[0].id) !== parseInt(auth.userId)) {
          await queryPg(
            `INSERT INTO notifications (user_id, type, title, body, from_user_id)
             VALUES ($1, 'like', $2, $3, $4)`,
            [parseInt(authorUser.rows[0].id),
             `${auth.username} 赞了你的视频`,
             `${auth.username} 赞了《${videoInfo.rows[0].title}》`,
             parseInt(auth.userId)]
          )
        }
      }
    } catch (e) {
      console.error('Failed to send like notification:', e)
    }

    return { liked: true, count: newCount }
  }
})
