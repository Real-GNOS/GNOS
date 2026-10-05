export default defineEventHandler(async (event) => {
  try {
    const slug = getRouterParam(event, 'slug')
    if (!slug) throw createError({ statusCode: 400, message: '缺少视频标识' })

    const video = await queryPg('SELECT id, slug, video_url, author FROM videos WHERE slug = $1', [slug])
    if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

    const auth = getAuthFromEvent(event)
    const userId = auth ? parseInt(auth.userId) : undefined
    const isOwner = auth ? video.rows[0].author === auth.username : false

    const { generateVideoToken } = await import('~/server/utils/video-token')
    const token = generateVideoToken(slug, userId, !!isOwner)

    return { success: true, token, videoUrl: video.rows[0].video_url, slug }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Video token error:', err)
    throw createError({ statusCode: 500, message: '获取视频失败' })
  }
})
