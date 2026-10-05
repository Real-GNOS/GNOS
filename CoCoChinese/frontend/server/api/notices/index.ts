import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const result = await queryPg(
    'SELECT id, title, content, link, created_at FROM notices WHERE is_active = true ORDER BY "order" ASC, created_at DESC'
  )
  return { success: true, notices: result.rows }
})
