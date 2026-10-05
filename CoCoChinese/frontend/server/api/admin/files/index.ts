import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'GET') {
    const query = getQuery(event)
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(100, Math.max(1, parseInt(query.size as string) || 20))
    const offset = (page - 1) * size
    const type = (query.type as string) || ''
    const search = (query.search as string) || ''

    let where = ''
    const params: any[] = []

    if (type) {
      where = 'WHERE m.file_type ILIKE $1'
      params.push(`${type}%`)
    }

    if (search) {
      const cond = type ? ' AND' : 'WHERE'
      if (type) {
        where += ` ${cond} (m.name ILIKE $${params.length + 1} OR m.file_path ILIKE $${params.length + 1})`
      } else {
        where = `WHERE (m.name ILIKE $1 OR m.file_path ILIKE $1)`
      }
      params.push(`%${search}%`)
    }

    const countResult = await queryPg(`SELECT count(*) FROM media_files m ${where}`, params)
    const total = parseInt(countResult.rows[0].count)

    params.push(size, offset)
    const result = await queryPg(
      `SELECT * FROM media_files m ${where} ORDER BY m.created_at DESC LIMIT $${params.length - 1} OFFSET $${params.length}`,
      params
    )

    // Get total storage
    const storageResult = await queryPg('SELECT COALESCE(SUM(file_size), 0) as total FROM media_files')
    const totalStorage = parseInt(storageResult.rows[0].total)

    return { success: true, total, page, size, files: result.rows, totalStorage }
  }

  throw createError({ statusCode: 405 })
})
