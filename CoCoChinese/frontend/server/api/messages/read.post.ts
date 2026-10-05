export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const body = await readBody(event)
    if (body && body.message_ids) {
      const ids = body.message_ids.join(',')
      await queryPg(`UPDATE messages SET is_read = true WHERE id IN (${ids}) AND to_user_id = $1`,
        [parseInt(auth.userId)])
    } else {
      await queryPg('UPDATE messages SET is_read = true WHERE to_user_id = $1',
        [parseInt(auth.userId)])
    }
    return { success: true }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Mark read error:', err)
    throw createError({ statusCode: 500, message: '标记已读失败' })
  }
})
