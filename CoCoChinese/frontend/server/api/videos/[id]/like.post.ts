export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const id = parseInt(getRouterParam(event, 'id') || '')
    if (isNaN(id)) throw createError({ statusCode: 400, message: '无效ID' })

    const video = await queryPg('SELECT id, like_volue, author FROM videos WHERE id = $1', [id])
    if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

    const existing = await queryPg(
      'SELECT id FROM video_likes WHERE user_id = $1 AND video_id = $2',
      [parseInt(auth.userId), id]
    )

    if (existing.rows.length) {
      await queryPg('DELETE FROM video_likes WHERE user_id = $1 AND video_id = $2',
        [parseInt(auth.userId), id])
      const newVal = Math.max(0, parseInt(video.rows[0].like_volue || '0') - 1)
      await queryPg('UPDATE videos SET like_volue = $1 WHERE id = $2', [String(newVal), id])
      return { liked: false, likeVolue: newVal }
    } else {
      await queryPg('INSERT INTO video_likes (user_id, video_id) VALUES ($1, $2)',
        [parseInt(auth.userId), id])
      const newVal = parseInt(video.rows[0].like_volue || '0') + 1
      await queryPg('UPDATE videos SET like_volue = $1 WHERE id = $2', [String(newVal), id])
      return { liked: true, likeVolue: newVal }
    }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Like error:', err)
    throw createError({ statusCode: 500, message: '操作失败' })
  }
})
