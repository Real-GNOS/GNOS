const SEGMENT_DURATION = 360

export default defineEventHandler(async (event) => {
  try {
    const slug = getRouterParam(event, 'slug')
    if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })

    const query = getQuery(event)
    const segment = query.segment !== undefined ? parseInt(query.segment as string) : null

    const where: any = { videoSlug: slug }
    if (segment !== null && !isNaN(segment)) {
      const from = segment * SEGMENT_DURATION
      const to = (segment + 1) * SEGMENT_DURATION
      where.time = { gte: from, lt: to }
    }

    const danmaku = await prisma.danmaku.findMany({
      where,
      orderBy: { time: 'asc' },
      take: 2000,
    })

    return { success: true, data: danmaku, segment }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Danmaku error:', err)
    throw createError({ statusCode: 500, message: '获取弹幕失败' })
  }
})
