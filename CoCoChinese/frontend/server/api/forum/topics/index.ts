import { requireAuth } from '../../../utils/pg'
import { getTopics, createTopic, generateUniqueSlug } from '../../../utils/forum'

export default defineEventHandler(async (event) => {
  const method = event.method

  if (method === 'GET') {
    const query = getQuery(event)
    const category = (query.category as string) || undefined
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const pageSize = Math.min(50, Math.max(1, parseInt(query.pageSize as string) || 20))
    const result = await getTopics(category, page, pageSize)
    return { success: true, data: result.topics, total: result.total, page, pageSize }
  }

  if (method === 'POST') {
    const auth = await requireAuth(event)
    const body = await readBody(event)
    if (!body || !body.title || !body.category_id || !body.content) {
      throw createError({ statusCode: 400, message: '缺少必要参数' })
    }
    const slug = await generateUniqueSlug('forum_topics')
    const topic = await createTopic({
      title: body.title.trim(),
      slug,
      category_id: parseInt(body.category_id),
      author_id: parseInt(auth.userId),
      content: body.content,
    })
    return { success: true, data: topic }
  }
})
