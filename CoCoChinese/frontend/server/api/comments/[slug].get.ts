export default defineEventHandler(async (event) => {
  try {
    const slug = getRouterParam(event, 'slug')
    if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })
    const comments = await getComments(slug)
    return { success: true, data: comments }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Comments error:', err)
    throw createError({ statusCode: 500, message: '获取评论失败' })
  }
})
