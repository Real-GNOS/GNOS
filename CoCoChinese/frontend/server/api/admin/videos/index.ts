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
    const status = query.status as string || ''

    let where = ''
    const params: any[] = []

    if (search) {
      where += 'WHERE (v.title ILIKE $1 OR v.author ILIKE $1)'
      params.push(`%${search}%`)
    }

    const countResult = await queryPg(`SELECT count(*) FROM videos v ${where}`, params)
    const total = parseInt(countResult.rows[0].count)

    params.push(size, offset)
    const result = await queryPg(
      `SELECT v.*, u.username as author_username, u.avatar_url as author_avatar
       FROM videos v
       LEFT JOIN users u ON u.username = v.author
       ${where}
       ORDER BY v.created_at DESC
       LIMIT $${params.length - 1} OFFSET $${params.length}`,
      params
    )

    return { success: true, total, page, size, videos: result.rows }
  } catch (err) {
    console.error('Admin videos error:', err)
    throw createError({ statusCode: 500, message: '获取视频列表失败' })
  }
})