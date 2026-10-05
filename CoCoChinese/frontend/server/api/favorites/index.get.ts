export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    
    
    const userId = parseInt(auth.userId)
    const query = getQuery(event)
    const folders = await getFavoriteFolders(userId)
    const favorites = query.folder_id
      ? await getUserFavorites(userId, parseInt(query.folder_id as string))
      : await getUserFavorites(userId)
    return { success: true, data: { folders, favorites } }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Favorites error:', err)
    throw createError({ statusCode: 500, message: '获取收藏失败' })
  }
})
