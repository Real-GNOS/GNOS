export default defineEventHandler(async (event) => {
  try {
    const id = parseInt(getRouterParam(event, 'id') || '')
    if (isNaN(id)) throw createError({ statusCode: 400, message: '无效ID' })
    const video = await prisma.video.findUnique({ where: { id } })
    if (!video) {
      throw createError({ statusCode: 404, message: '未找到该视频' })
    }
    return toLegacyVideo(video)
  } catch (err) {
    if (err.statusCode) throw err
    console.error(err)
    throw createError({ statusCode: 500, message: '服务器错误' })
  }
})
