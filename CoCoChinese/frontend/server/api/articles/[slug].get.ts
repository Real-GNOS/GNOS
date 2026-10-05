export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  if (!slug) throw createError({ statusCode: 400, message: '缺少slug' })
  const post = await getPostBySlug(slug)
  if (!post) throw createError({ statusCode: 404, message: '文章不存在' })
  return { success: true, data: post }
})
