export default defineEventHandler(async (event) => {
  try {
    const query = getQuery(event)
    const where: any = {}

    if (query.author) {
      where.author = query.author
    }
    if (query.category) {
      where.videoType = query.category
    }

    if (query.exclude) {
      where.slug = { not: query.exclude }
    }

    const orderBy: any[] = [{ order: 'asc' }, { createdAt: 'desc' }]
    if (query.sort === 'popular') {
      orderBy.unshift({ watchVolue: 'desc' })
    }

    const take = parseInt(query.take) || 50

    const rows = (await prisma.video.findMany({
      where,
      orderBy,
      take: Math.min(take, 100),
    })).map(toLegacyVideo)

    const result = query.sessionId
      ? rows.map(r => encryptResponseUrls(r, query.sessionId as string))
      : rows

    return { success: true, rows: result }
  } catch (err) {
    console.error(err)
    throw createError({ statusCode: 500, message: '服务器错误' })
  }
})
