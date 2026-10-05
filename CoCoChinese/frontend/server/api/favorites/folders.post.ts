export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const body = await readBody(event)
    if (!body || !body.name) throw createError({ statusCode: 400, message: '收藏夹名称不能为空' })
    await queryPg(
      'INSERT INTO favorite_folders (user_id, name, description, is_public) VALUES ($1, $2, $3, $4)',
      [parseInt(auth.userId), body.name, body.description || '', body.is_public !== false])
    return { success: true, message: '收藏夹创建成功' }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Create folder error:', err)
    throw createError({ statusCode: 500, message: '创建收藏夹失败' })
  }
})
