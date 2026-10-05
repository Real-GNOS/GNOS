export default defineEventHandler(async () => {
  const { queryPg } = await import('../../utils/pg')
  const result = await queryPg('SELECT COUNT(*) FROM comments')
  return { total: parseInt(result.rows[0].count) }
})
