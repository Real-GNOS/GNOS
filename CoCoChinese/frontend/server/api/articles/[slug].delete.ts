export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const slug = getRouterParam(event, 'slug')
  if (!slug) throw createError({ statusCode: 400, message: '缺少slug' })
  const existing = await getPostBySlug(slug)
  if (!existing) throw createError({ statusCode: 404, message: '文章不存在' })
  if (existing.author !== auth.username && auth.role !== 'admin') {
    throw createError({ statusCode: 403, message: '只能删除自己的文章' })
  }
  await deletePost(slug)
  return { success: true, message: '删除成功' }
})
