export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const { bannerUrl } = await readBody(event)
  if (!bannerUrl) throw createError({ statusCode: 400, message: '缺少封面图URL' })

  const { queryPg } = await import('../../utils/pg')
  await queryPg(`
    INSERT INTO user_banners (user_id, banner_url) VALUES ($1, $2)
    ON CONFLICT (user_id) DO UPDATE SET banner_url = EXCLUDED.banner_url, updated_at = NOW()
  `, [auth.userId, bannerUrl])

  return { success: true }
})
