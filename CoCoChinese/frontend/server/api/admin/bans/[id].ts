import { requireAdmin } from '~/server/utils/pg'
import { getRouterParam } from 'h3'
import { updateBan, deleteBan, getBanById } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = getRouterParam(event, 'id')

  if (event.method === 'GET') {
    const ban = await getBanById(parseInt(id))
    if (!ban) throw createError({ statusCode: 404, message: '封禁记录不存在' })
    return { success: true, data: ban }
  }

  if (event.method === 'PUT') {
    const body = await readBody(event)
    const { isActive, reason, duration } = body
    const ban = await updateBan(parseInt(id), { isActive, reason, duration })
    return { success: true, data: ban }
  }

  if (event.method === 'DELETE') {
    await deleteBan(parseInt(id))
    return { success: true, message: '已删除' }
  }

  throw createError({ statusCode: 405, message: 'Method not allowed' })
})