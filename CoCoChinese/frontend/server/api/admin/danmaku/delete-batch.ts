import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'POST') {
    const body = await readBody(event)
    const { ids } = body

    if (!ids || !Array.isArray(ids) || !ids.length) {
      throw createError({ statusCode: 400, message: '请选择要删除的弹幕' })
    }

    const placeholders = ids.map((_: number, i: number) => `$${i + 1}`).join(',')
    await queryPg(`DELETE FROM danmaku WHERE id IN (${placeholders})`, ids)

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, details) VALUES ($1, $2, $3, $4)',
      [auth.userId, 'batch_delete_danmaku', 'danmaku', `批量删除 ${ids.length} 条弹幕`]
    )

    return { success: true, deleted: ids.length }
  }

  throw createError({ statusCode: 405 })
})
