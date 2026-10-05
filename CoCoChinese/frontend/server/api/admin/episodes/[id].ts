import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (!id) throw createError({ statusCode: 400, message: '无效ID' })

  if (event.method === 'PUT') {
    const body = await readBody(event)
    const { title, video_url, image_url, duration, season, episode, sort_order } = body

    const updates: string[] = []
    const params: any[] = []
    let idx = 1

    if (title !== undefined) { updates.push(`title = $${idx++}`); params.push(title) }
    if (video_url !== undefined) { updates.push(`video_url = $${idx++}`); params.push(video_url) }
    if (image_url !== undefined) { updates.push(`image_url = $${idx++}`); params.push(image_url) }
    if (duration !== undefined) { updates.push(`duration = $${idx++}`); params.push(duration) }
    if (season !== undefined) { updates.push(`season = $${idx++}`); params.push(season) }
    if (episode !== undefined) { updates.push(`episode = $${idx++}`); params.push(episode) }
    if (sort_order !== undefined) { updates.push(`sort_order = $${idx++}`); params.push(sort_order) }

    if (!updates.length) throw createError({ statusCode: 400, message: '无更新内容' })

    params.push(id)
    await queryPg(`UPDATE video_episodes SET ${updates.join(', ')} WHERE id = $${idx}`, params)

    return { success: true }
  }

  if (event.method === 'DELETE') {
    await queryPg('DELETE FROM video_episodes WHERE id = $1', [id])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'delete_episode', 'episode', id, `删除剧集 #${id}`]
    )

    return { success: true }
  }

  if (event.method === 'PATCH') {
    // Reorder: accept array of { id, sort_order }
    const body = await readBody(event)
    const { orders } = body

    if (!orders || !Array.isArray(orders)) {
      throw createError({ statusCode: 400, message: 'orders must be an array' })
    }

    for (const item of orders) {
      await queryPg('UPDATE video_episodes SET sort_order = $1 WHERE id = $2', [item.sort_order, item.id])
    }

    return { success: true }
  }

  throw createError({ statusCode: 405 })
})
