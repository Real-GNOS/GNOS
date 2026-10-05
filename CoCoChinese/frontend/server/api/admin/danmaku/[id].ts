import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (!id) throw createError({ statusCode: 400, message: '无效ID' })

  if (event.method === 'DELETE') {
    await queryPg('DELETE FROM danmaku WHERE id = $1', [id])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'delete_danmaku', 'danmaku', id, `删除弹幕 #${id}`]
    )

    return { success: true }
  }

  throw createError({ statusCode: 405 })
})
