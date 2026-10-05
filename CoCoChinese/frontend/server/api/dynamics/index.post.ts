export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const body = await readBody(event)
    if (!body || !body.type) throw createError({ statusCode: 400, message: '参数错误' })
    await queryPg(
      'INSERT INTO activities (user_id, type, target_id, content) VALUES ($1, $2, $3, $4)',
      [parseInt(auth.userId), body.type, body.target_id || null, body.content || ''])
    return { success: true, message: '发布成功' }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Create activity error:', err)
    throw createError({ statusCode: 500, message: '发布失败' })
  }
})
