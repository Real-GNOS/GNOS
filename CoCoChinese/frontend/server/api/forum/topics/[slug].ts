import { getTopicBySlug, getForumPosts, incrementTopicView } from '../../../utils/forum'

export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })

  const topic = await getTopicBySlug(slug)
  if (!topic) throw createError({ statusCode: 404, message: '话题不存在' })

  await incrementTopicView(slug)

  const query = getQuery(event)
  const page = Math.max(1, parseInt(query.page as string) || 1)
  const pageSize = Math.min(50, Math.max(1, parseInt(query.pageSize as string) || 20))

  const { posts, total } = await getForumPosts(slug, page, pageSize)

  return { success: true, data: { topic, posts }, total, page, pageSize }
})
