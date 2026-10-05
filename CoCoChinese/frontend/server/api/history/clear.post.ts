export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    await queryPg('DELETE FROM watch_history WHERE user_id = $1', [parseInt(auth.userId)])
    return { success: true, message: '已清空观看历史' }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Clear history error:', err)
    throw createError({ statusCode: 500, message: '清空失败' })
  }
})
