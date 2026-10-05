export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const id = parseInt(getRouterParam(event, 'id') || '')
    if (isNaN(id)) throw createError({ statusCode: 400, message: '无效ID' })

    const video = await prisma.video.findUnique({ where: { id }, select: { author: true } })
    if (!video) throw createError({ statusCode: 404, message: '视频不存在' })

    if (video.author !== auth.username) {
      throw createError({ statusCode: 403, message: '无权编辑此视频' })
    }

    return { success: true, isOwner: true }
  } catch (err) {
    if (err.statusCode) throw err
    throw createError({ statusCode: 500, message: '验证失败' })
  }
})
