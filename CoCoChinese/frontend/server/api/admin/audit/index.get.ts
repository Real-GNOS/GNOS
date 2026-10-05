export default defineEventHandler(async (event) => {
  await requireAdmin(event)
  const { page = '1', size = '50', action } = getQuery(event)
  const { queryPg } = await import('../../../utils/pg')

  const offset = (parseInt(page as string) - 1) * parseInt(size as string)
  let where = ''
  const params: any[] = []

  if (action) {
    params.push(action)
    where = `WHERE a.action = $${params.length}`
  }

  const countRes = await queryPg(`SELECT COUNT(*) FROM audit_logs a ${where}`, params)
  const total = parseInt(countRes.rows[0].count)

  params.push(parseInt(size as string), offset)
  const logs = await queryPg(`
    SELECT a.*, u.username, u.avatar_url
    FROM audit_logs a
    LEFT JOIN users u ON u.id = a.user_id
    ${where}
    ORDER BY a.created_at DESC
    LIMIT $${params.length - 1} OFFSET $${params.length}
  `, params)

  return { total, logs: logs.rows }
})
