export default defineEventHandler(async (event) => {
  try {
    const id = parseInt(getRouterParam(event, 'id') || '')
    if (isNaN(id)) throw createError({ statusCode: 400, message: '无效ID' })

    const auth = getAuthFromEvent(event)
    if (!auth) return { liked: false }

    const existing = await queryPg(
      'SELECT id FROM video_likes WHERE user_id = $1 AND video_id = $2',
      [parseInt(auth.userId), id])
    return { liked: existing.rows.length > 0 }
  } catch (err) {
    if (err.statusCode) throw err
    return { liked: false }
  }
})
