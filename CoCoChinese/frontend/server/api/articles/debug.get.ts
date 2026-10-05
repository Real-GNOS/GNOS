export default defineEventHandler(async () => {
  try {
    const result = await queryPg('SELECT slug, title FROM posts ORDER BY created_at DESC LIMIT 10')
    return { success: true, posts: result.rows }
  } catch (e) {
    return { success: false, error: String(e) }
  }
})
