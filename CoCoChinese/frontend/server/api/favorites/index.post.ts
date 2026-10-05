export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const body = await readBody(event)
    if (!body || !body.video_id) throw createError({ statusCode: 400, message: '缺少视频ID' })
    const userId = parseInt(auth.userId)
    const existing = await queryPg(
      'SELECT id FROM favorites WHERE user_id = $1 AND video_id = $2',
      [userId, body.video_id])
    if (existing.rows.length) {
      await queryPg('DELETE FROM favorites WHERE id = $1', [existing.rows[0].id])
      return { success: true, message: '已取消收藏', favorited: false }
    }
    await queryPg(
      'INSERT INTO favorites (user_id, video_id, folder_id) VALUES ($1, $2, $3)',
      [userId, body.video_id, body.folder_id || null])
    return { success: true, message: '收藏成功', favorited: true }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Favorite error:', err)
    throw createError({ statusCode: 500, message: '收藏失败' })
  }
})
