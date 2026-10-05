export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const history = await getUserHistory(parseInt(auth.userId))
    return { success: true, data: history }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('History error:', err)
    throw createError({ statusCode: 500, message: '获取历史失败' })
  }
})
