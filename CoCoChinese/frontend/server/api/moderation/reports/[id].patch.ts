export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const id = parseInt(getRouterParam(event, 'id')!)
  const { status } = await readBody(event)

  if (!['approved', 'rejected'].includes(status)) {
    throw createError({ statusCode: 400, message: '无效的状态' })
  }

  const { queryPg } = await import('../../../utils/pg')
  await queryPg(
    'UPDATE content_reports SET status = $1, handled_by_id = $2, handled_at = NOW() WHERE id = $3',
    [status, auth.userId, id]
  )

  return { success: true }
})
