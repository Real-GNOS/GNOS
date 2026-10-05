export default defineEventHandler(async (event) => {
  await requireAdmin(event)
  const { queryPg } = await import('../../../utils/pg')

  const configs = await queryPg('SELECT * FROM site_configs ORDER BY key')
  const result: Record<string, string> = {}
  for (const row of configs.rows) {
    result[row.key] = row.value
  }

  return result
})
