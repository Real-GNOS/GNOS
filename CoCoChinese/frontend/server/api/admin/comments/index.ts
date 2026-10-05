import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const query = getQuery(event)

  try {
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(100, Math.max(1, parseInt(query.size as string) || 20))
    const offset = (page - 1) * size
    const search = query.search as string || ''

    let where = ''
    const params: any[] = []

    if (search) {
      where = 'WHERE c.content ILIKE $1 OR c.username ILIKE $1'
      params.push(`%${search}%`)
    }

    const countResult = await queryPg(
      `SELECT count(*) FROM comments c ${where}`, params)
    const total = parseInt(countResult.rows[0].count)

    params.push(size, offset)
    const result = await queryPg(
      `SELECT c.*, v.title as video_title, v.slug as video_slug
       FROM comments c
       LEFT JOIN videos v ON v.slug = c.video_slug
       ${where}
       ORDER BY c.created_at DESC
       LIMIT $${params.length - 1} OFFSET $${params.length}`,
      params
    )

    return { success: true, total, page, size, comments: result.rows }
  } catch (err) {
    console.error('Admin comments error:', err)
    throw createError({ statusCode: 500, message: '获取评论列表失败' })
  }
})