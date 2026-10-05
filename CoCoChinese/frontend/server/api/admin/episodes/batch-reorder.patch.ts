import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'PATCH') {
    const body = await readBody(event)
    const { orders } = body

    if (!orders || !Array.isArray(orders)) {
      throw createError({ statusCode: 400, message: 'orders must be an array' })
    }

    for (const item of orders) {
      if (!item.id || item.sort_order === undefined) continue
      await queryPg('UPDATE video_episodes SET sort_order = $1 WHERE id = $2', [item.sort_order, item.id])
    }

    return { success: true }
  }

  throw createError({ statusCode: 405 })
})
