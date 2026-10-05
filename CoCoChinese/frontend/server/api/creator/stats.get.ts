export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const stats = await getCreatorStats(parseInt(auth.userId))
    const profile = await getUserProfile(parseInt(auth.userId))
    return { success: true, data: { ...stats, followers: profile?.followers || 0 } }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Creator stats error:', err)
    throw createError({ statusCode: 500, message: '获取创作数据失败' })
  }
})
