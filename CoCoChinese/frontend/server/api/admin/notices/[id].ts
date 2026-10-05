import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (!id) throw createError({ statusCode: 400, message: '无效ID' })

  if (event.method === 'PUT') {
    const body = await readBody(event)
    const { title, content, link, is_active, order } = body

    const updates: string[] = []
    const params: any[] = []
    let idx = 1

    if (title !== undefined) { updates.push(`title = $${idx++}`); params.push(title) }
    if (content !== undefined) { updates.push(`content = $${idx++}`); params.push(content) }
    if (link !== undefined) { updates.push(`link = $${idx++}`); params.push(link) }
    if (is_active !== undefined) { updates.push(`is_active = $${idx++}`); params.push(!!is_active) }
    if (order !== undefined) { updates.push(`"order" = $${idx++}`); params.push(order) }

    if (!updates.length) throw createError({ statusCode: 400, message: '无更新内容' })

    params.push(id)
    await queryPg(`UPDATE notices SET ${updates.join(', ')} WHERE id = $${idx}`, params)

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'update_notice', 'notice', id, `更新公告: ${title || id}`]
    )

    return { success: true }
  }

  if (event.method === 'DELETE') {
    await queryPg('DELETE FROM notices WHERE id = $1', [id])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'delete_notice', 'notice', id, `删除公告 #${id}`]
    )

    return { success: true }
  }

  throw createError({ statusCode: 405 })
})
