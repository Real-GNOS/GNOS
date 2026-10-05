export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { targetType, targetId, reason, description } = await readBody(event)

  if (!targetType || !targetId || !reason) {
    throw createError({ statusCode: 400, message: '参数不完整' })
  }

  const { queryPg } = await import('../../utils/pg')
  await queryPg(
    'INSERT INTO content_reports (reporter_id, target_type, target_id, reason, description) VALUES ($1, $2, $3, $4, $5)',
    [auth.userId, targetType, targetId, reason, description || '']
  )

  return { success: true, message: '举报已提交' }
})
