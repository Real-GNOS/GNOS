export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const slug = getRouterParam(event, 'slug')
  if (!slug) throw createError({ statusCode: 400, message: '缺少slug' })
  const existing = await getPostBySlug(slug)
  if (!existing) throw createError({ statusCode: 404, message: '文章不存在' })
  if (existing.author !== auth.username && auth.role !== 'admin') {
    throw createError({ statusCode: 403, message: '只能编辑自己的文章' })
  }
  const body = await readBody(event)
  const updated = await updatePost(slug, {
    title: body.title?.trim(),
    content: body.content,
    cover_image: body.cover_image,
    category: body.category,
    tags: body.tags,
    status: body.status,
  })

  // 每次保存/发布自动在 git 里留一个新版本
  if (updated) {
    try {
      await commitCurrentPostVersion(
        slug,
        body.message || `更新文章「${updated.title}」`,
        parseInt(auth.userId, 10),
        auth.username
      )
    } catch (err) {
      console.error('update article version failed:', err)
    }
  }

  return { success: true, data: updated }
})
