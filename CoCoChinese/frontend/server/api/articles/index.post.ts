export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const body = await readBody(event)
  if (!body?.title?.trim()) throw createError({ statusCode: 400, message: '标题不能为空' })
  const slug = await uniqueSlug('posts')
  const post = await createPost({
    slug,
    title: body.title.trim(),
    content: body.content || '',
    type: body.type || 'article',
    author: auth.username,
    author_img: auth.avatar_url || '',
    cover_image: body.cover_image || '',
    category: body.category || '',
    tags: body.tags || [],
    images: body.images || [],
    status: body.status || 'published',
  })

  // 初始版本 v1：git 里留下这篇文章的第一份快照
  try {
    await commitCurrentPostVersion(slug, `创建文章「${post.title}」`, parseInt(auth.userId, 10), auth.username)
  } catch (err) {
    console.error('create article initial version failed:', err)
  }

  return { success: true, data: post }
})
