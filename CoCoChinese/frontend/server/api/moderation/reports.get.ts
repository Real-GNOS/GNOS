export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const { status, page = '1', size = '20' } = getQuery(event)
  const { queryPg } = await import('../../utils/pg')

  const offset = (parseInt(page as string) - 1) * parseInt(size as string)
  let where = ''
  const params: any[] = []

  if (status && status !== 'all') {
    params.push(status)
    where = `WHERE r.status = $${params.length}`
  }

  const countRes = await queryPg(`SELECT COUNT(*) FROM content_reports r ${where}`, params)
  const total = parseInt(countRes.rows[0].count)

  params.push(parseInt(size as string), offset)
  const reports = await queryPg(`
    SELECT r.*, u.username AS reporter_name, u.avatar_url AS reporter_avatar,
           h.username AS handler_name
    FROM content_reports r
    LEFT JOIN users u ON u.id = r.reporter_id
    LEFT JOIN users h ON h.id = r.handled_by_id
    ${where}
    ORDER BY r.created_at DESC
    LIMIT $${params.length - 1} OFFSET $${params.length}
  `, params)

  return { total, reports: reports.rows }
})
