export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const rows = await prisma.video.findMany({
      where: { author: auth.username },
      orderBy: { createdAt: 'desc' },
      take: 100,
    })
    return { success: true, rows: rows.map(toLegacyVideo) }
  } catch (err) {
    if (err.statusCode) throw err
    throw createError({ statusCode: 500, message: '获取视频列表失败' })
  }
})
