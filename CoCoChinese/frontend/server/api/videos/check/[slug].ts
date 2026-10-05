export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  if (!slug || slug.length < 4 || slug.length > 20) {
    throw createError({ statusCode: 400, message: '无效的视频ID' })
  }
  const r = await queryPg(
    `SELECT slug, title, author, status FROM videos WHERE slug = $1`,
    [slug]
  )
  if (!r.rows.length) {
    throw createError({ statusCode: 404, message: '视频不存在' })
  }
  const video = r.rows[0]
  if (video.status === 'banned' || video.status === 'deleted') {
    throw createError({ statusCode: 403, message: '视频已被屏蔽或删除' })
  }
  return {
    success: true,
    data: { slug: video.slug, title: video.title, author: video.author }
  }
})
