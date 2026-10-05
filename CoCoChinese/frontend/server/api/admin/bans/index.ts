import { requireAdmin } from '~/server/utils/pg'
import { getBanList, createBan, findUserById, findUserByUsername } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const query = getQuery(event)

  if (event.method === 'GET') {
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(100, Math.max(1, parseInt(query.size as string) || 20))
    const status = query.status as string || undefined
    const result = await getBanList(page, size, status)
    return { success: true, ...result }
  }

  if (event.method === 'POST') {
    const body = await readBody(event)
    const { userId, type, reason, duration } = body
    if (!userId || !type || !reason) {
      throw createError({ statusCode: 400, message: '缺少必要参数' })
    }

    // userId 可以是数字 ID，也可以是用户名——表单允许两种输入
    const raw = String(userId).trim()
    let target: any
    if (/^\d+$/.test(raw)) {
      target = await findUserById(parseInt(raw, 10))
    } else {
      target = await findUserByUsername(raw)
    }
    if (!target) throw createError({ statusCode: 404, message: '用户不存在' })
    const targetId = target.id

    // 权限校验：管理员（含超级管理员）不可被封禁；不能封禁自己
    if (target.id === auth.userId) {
      throw createError({ statusCode: 403, message: '不能封禁自己' })
    }
    if (target.role === 'admin' || target.role === 'super_admin') {
      throw createError({ statusCode: 403, message: '管理员账号不可被封禁' })
    }

    const ban = await createBan(targetId, auth.userId, type, reason, duration)
    return { success: true, data: ban }
  }

  throw createError({ statusCode: 405, message: 'Method not allowed' })
})