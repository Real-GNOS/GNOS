export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const body = await readBody(event)
    if (!body || !body.video_id) throw createError({ statusCode: 400, message: '缺少视频ID' })
    await queryPg(
      'DELETE FROM watch_history WHERE user_id = $1 AND video_id = $2',
      [parseInt(auth.userId), body.video_id])
    await queryPg(
      'INSERT INTO watch_history (user_id, video_id, progress) VALUES ($1, $2, $3)',
      [parseInt(auth.userId), body.video_id, body.progress || 0])
    return { success: true }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('History add error:', err)
    throw createError({ statusCode: 500, message: '记录失败' })
  }
})
