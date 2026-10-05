import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'GET') {
    const query = getQuery(event)
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(200, Math.max(1, parseInt(query.size as string) || 50))
    const offset = (page - 1) * size
    const search = (query.search as string) || ''
    const videoSlug = (query.video_slug as string) || ''

    let where = ''
    const params: any[] = []

    if (search) {
      where = 'WHERE (d.content ILIKE $1 OR d.username ILIKE $1)'
      params.push(`%${search}%`)
    }

    if (videoSlug) {
      where = where ? `${where} AND d.video_slug = $${params.length + 1}` : 'WHERE d.video_slug = $1'
      params.push(videoSlug)
    }

    const countResult = await queryPg(`SELECT count(*) FROM danmaku d ${where}`, params)
    const total = parseInt(countResult.rows[0].count)

    params.push(size, offset)
    const result = await queryPg(
      `SELECT d.*, v.title as video_title
       FROM danmaku d
       LEFT JOIN videos v ON v.slug = d.video_slug
       ${where}
       ORDER BY d.created_at DESC
       LIMIT $${params.length - 1} OFFSET $${params.length}`,
      params
    )

    return { success: true, total, page, size, danmaku: result.rows }
  }

  throw createError({ statusCode: 405 })
})
