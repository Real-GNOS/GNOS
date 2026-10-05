import { requireAuth } from '~/server/utils/pg'
import { queryPg, getUserStats } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    console.log('[stats] auth:', JSON.stringify(auth))
    const userId = parseInt(auth.userId)
    console.log('[stats] userId:', userId)
    if (isNaN(userId)) throw createError({ statusCode: 401, message: '无效的用户标识' })
    // 确保 profile 存在
    await queryPg('INSERT INTO user_profiles (user_id) VALUES ($1) ON CONFLICT DO NOTHING', [userId])
    const stats = await getUserStats(userId)
    return { success: true, data: stats }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Stats error:', err)
    throw createError({ statusCode: 500, message: '获取统计数据失败', data: String(err) })
  }
})
