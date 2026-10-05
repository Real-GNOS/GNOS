import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'GET') {
    const query = getQuery(event)
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(100, Math.max(1, parseInt(query.size as string) || 20))
    const offset = (page - 1) * size

    const countResult = await queryPg('SELECT count(*) FROM notices')
    const total = parseInt(countResult.rows[0].count)

    const result = await queryPg(
      'SELECT * FROM notices ORDER BY "order" ASC, created_at DESC LIMIT $1 OFFSET $2',
      [size, offset]
    )

    return { success: true, total, page, size, notices: result.rows }
  }

  if (event.method === 'POST') {
    const body = await readBody(event)
    const { title, content, link, is_active, order } = body
    if (!title) throw createError({ statusCode: 400, message: '标题不能为空' })

    const result = await queryPg(
      `INSERT INTO notices (title, content, link, is_active, "order")
       VALUES ($1, $2, $3, $4, $5) RETURNING *`,
      [title, content || '', link || '', is_active === undefined ? true : !!is_active, order || 0]
    )

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'create_notice', 'notice', result.rows[0].id, `发布公告: ${title}`]
    )

    return { success: true, data: result.rows[0] }
  }

  throw createError({ statusCode: 405 })
})
