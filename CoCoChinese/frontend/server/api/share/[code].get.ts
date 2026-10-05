export default defineEventHandler(async (event) => {
  const code = getRouterParam(event, 'code')
  const { queryPg } = await import('../../utils/pg')

  const link = await queryPg('SELECT * FROM share_links WHERE code = $1', [code])
  if (!link.rows.length) {
    throw createError({ statusCode: 404, message: '分享链接不存在' })
  }

  const siteUrl = process.env.SITE_URL || 'http://localhost:3000'
  let url = '/'

  switch (link.rows[0].target_type) {
    case 'video':
      url = `${siteUrl}/player/${link.rows[0].target_id}`
      break
    case 'post':
      url = `${siteUrl}/posts/${link.rows[0].target_id}`
      break
    case 'playlist':
      url = `${siteUrl}/playlists/${link.rows[0].target_id}`
      break
  }

  return { url }
})
