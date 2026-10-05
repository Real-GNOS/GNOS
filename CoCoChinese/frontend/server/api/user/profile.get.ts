import { requireAuth } from '~/server/utils/pg'
import { queryPg, getUserProfile } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    console.log('[profile] auth:', JSON.stringify(auth))
    const userId = parseInt(auth.userId)
    console.log('[profile] userId:', userId)
    if (isNaN(userId)) throw createError({ statusCode: 401, message: '无效的用户标识' })
    let profile = await getUserProfile(userId)
    if (!profile) {
      // 创建默认 profile
      await queryPg('INSERT INTO user_profiles (user_id) VALUES ($1) ON CONFLICT DO NOTHING', [userId])
      profile = await getUserProfile(userId)
    }
    if (!profile) throw createError({ statusCode: 404, message: '用户不存在' })
    return { success: true, data: profile }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Profile error:', err)
    throw createError({ statusCode: 500, message: '获取用户信息失败', data: String(err) })
  }
})
