export default defineEventHandler(async (event) => {
  const query = getQuery(event)
  const posts = await getPosts({
    type: query.type as string || 'article',
    author: query.author as string || undefined,
    limit: parseInt(query.limit as string) || 20,
    offset: parseInt(query.offset as string) || 0,
  })
  return { success: true, data: posts }
})
