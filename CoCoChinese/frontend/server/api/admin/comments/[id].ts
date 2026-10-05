import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id') || '')

  if (event.method === 'DELETE') {
    try {
      await queryPg('DELETE FROM comments WHERE id = $1', [id])
      return { success: true }
    } catch (err) {
      throw createError({ statusCode: 500, message: '删除失败' })
    }
  }

  throw createError({ statusCode: 405 })
})