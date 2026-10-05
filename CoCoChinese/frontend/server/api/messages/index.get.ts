export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const query = getQuery(event)
    const type = (query.type as string) || undefined
    const messages = await getUserMessages(parseInt(auth.userId), type === 'all' ? undefined : type)
    return { success: true, data: messages }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Messages error:', err)
    throw createError({ statusCode: 500, message: '获取消息失败' })
  }
})
