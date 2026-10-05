export default defineEventHandler(async (event) => {
  try {
    const query = getQuery(event)
    const auth = getAuthFromEvent(event)
    let activities
    if (auth) {
      const userId = query.user_id ? parseInt(query.user_id as string) : parseInt(auth.userId)
      activities = await getActivities(userId)
    } else {
      activities = await getActivities()
    }
    return { success: true, data: activities }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Dynamics error:', err)
    throw createError({ statusCode: 500, message: '获取动态失败' })
  }
})
