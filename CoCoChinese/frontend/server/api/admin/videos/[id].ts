import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (event.method === 'GET') {
    try {
      const result = await queryPg('SELECT * FROM videos WHERE id = $1', [id])
      if (!result.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })
      return { success: true, video: result.rows[0] }
    } catch (err) {
      if (err.statusCode) throw err
      throw createError({ statusCode: 500, message: '获取视频失败' })
    }
  }

  if (event.method === 'PUT') {
    try {
      const body = await readBody(event)
      const { title, description, video_type, category, tags, image_url, is_deleted } = body

      const updates: string[] = []
      const params: any[] = []
      let idx = 1

      if (title !== undefined) { updates.push(`title = $${idx++}`); params.push(title) }
      if (description !== undefined) { updates.push(`description = $${idx++}`); params.push(description) }
      if (video_type !== undefined) { updates.push(`video_type = $${idx++}`); params.push(video_type) }
      if (category !== undefined) { updates.push(`category = $${idx++}`); params.push(category) }
      if (tags !== undefined) { updates.push(`tags = $${idx++}`); params.push(tags) }
      if (image_url !== undefined) { updates.push(`image_url = $${idx++}`); params.push(image_url) }
      if (is_deleted !== undefined) { updates.push(`is_deleted = $${idx++}`); params.push(is_deleted) }

      if (!updates.length) throw createError({ statusCode: 400, message: '无更新内容' })

      updates.push('updated_at = CURRENT_TIMESTAMP')
      params.push(id)

      await queryPg(`UPDATE videos SET ${updates.join(', ')} WHERE id = $${idx}`, params)

      return { success: true }
    } catch (err) {
      if (err.statusCode) throw err
      throw createError({ statusCode: 500, message: '更新失败' })
    }
  }

  if (event.method === 'DELETE') {
    try {
      await queryPg('DELETE FROM videos WHERE id = $1', [id])
      return { success: true }
    } catch (err) {
      throw createError({ statusCode: 500, message: '删除失败' })
    }
  }

  throw createError({ statusCode: 405 })
})