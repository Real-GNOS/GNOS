import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { sanitizeCarousel } from '~/server/utils/carouselValidate'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (!id) throw createError({ statusCode: 400, message: '无效ID' })

  if (event.method === 'PUT') {
    const body = await readBody(event)
    const data = sanitizeCarousel(body)
    const { image_url, link, title, author, watch_volue, like_volue, introduction, video_time, description, order } =
      data as Record<string, any>

    const updates: string[] = []
    const params: any[] = []
    let idx = 1

    if (image_url !== undefined) { updates.push(`image_url = $${idx++}`); params.push(image_url) }
    if (link !== undefined) { updates.push(`link = $${idx++}`); params.push(link) }
    if (title !== undefined) { updates.push(`title = $${idx++}`); params.push(title) }
    if (author !== undefined) { updates.push(`author = $${idx++}`); params.push(author) }
    if (watch_volue !== undefined) { updates.push(`watch_volue = $${idx++}`); params.push(watch_volue) }
    if (like_volue !== undefined) { updates.push(`like_volue = $${idx++}`); params.push(like_volue) }
    if (introduction !== undefined) { updates.push(`introduction = $${idx++}`); params.push(introduction) }
    if (video_time !== undefined) { updates.push(`video_time = $${idx++}`); params.push(video_time) }
    if (description !== undefined) { updates.push(`description = $${idx++}`); params.push(description) }
    if (order !== undefined) { updates.push(`"order" = $${idx++}`); params.push(order) }

    if (!updates.length) throw createError({ statusCode: 400, message: '无更新内容' })

    params.push(id)
    await queryPg(`UPDATE carousels SET ${updates.join(', ')} WHERE id = $${idx}`, params)

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'update_carousel', 'carousel', id, `更新轮播图: ${title || id}`]
    )

    return { success: true }
  }

  if (event.method === 'DELETE') {
    await queryPg('DELETE FROM carousels WHERE id = $1', [id])

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'delete_carousel', 'carousel', id, `删除轮播图 #${id}`]
    )

    return { success: true }
  }

  throw createError({ statusCode: 405 })
})
