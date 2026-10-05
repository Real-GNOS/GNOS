import { findUserById } from '../../../utils/pg'

export default defineEventHandler(async (event) => {
  try {
    const id = parseInt(getRouterParam(event, 'id') || '')
    if (!id) throw createError({ statusCode: 400, message: '缺少用户ID' })

    const user = await findUserById(id)
    if (!user) throw createError({ statusCode: 404, message: '用户不存在' })

    return { success: true, user: { id: user.id, username: user.username, display_name: user.display_name, avatar_url: user.avatar_url } }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Get user by id error:', err)
    throw createError({ statusCode: 500, message: '获取用户信息失败' })
  }
})
