export default defineEventHandler(async (event) => {
  try {
    const query = getQuery(event)
    const q = (query.q as string || '').trim()
    if (!q) {
      return { success: true, total: 0, results: [] }
    }
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(50, Math.max(1, parseInt(query.size as string) || 20))
    const from = (page - 1) * size

    const { q: _, page: _p, size: _s, ...filters } = query
    const { searchVideos } = await import('~/server/utils/search')
    const result = await searchVideos(q, from, size)

    return {
      success: true,
      query: q,
      total: result.total,
      page,
      size,
      results: result.results,
    }
  } catch (err) {
    console.error('Search API error:', err)
    throw createError({ statusCode: 500, message: '搜索失败' })
  }
})
