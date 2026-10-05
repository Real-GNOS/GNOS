import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { sanitizeCarousel } from '~/server/utils/carouselValidate'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'GET') {
    const query = getQuery(event)
    const page = Math.max(1, parseInt(query.page as string) || 1)
    const size = Math.min(100, Math.max(1, parseInt(query.size as string) || 20))
    const offset = (page - 1) * size

    const countResult = await queryPg('SELECT count(*) FROM carousels')
    const total = parseInt(countResult.rows[0].count)

    const result = await queryPg(
      'SELECT * FROM carousels ORDER BY "order" ASC, created_at DESC LIMIT $1 OFFSET $2',
      [size, offset]
    )

    return { success: true, total, page, size, carousels: result.rows }
  }

  if (event.method === 'POST') {
    const body = await readBody(event)
    const data = sanitizeCarousel(body, { requireImage: true })
    const { image_url, link, title, author, author_img, watch_volue, like_volue, introduction, video_time, description, order } =
      data as Record<string, any>

    const result = await queryPg(
      `INSERT INTO carousels (image_url, link, title, author, author_img, watch_volue, like_volue, introduction, video_time, description, "order")
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) RETURNING *`,
      [image_url || '', link || '', title || '', author || '', author_img || '',
       watch_volue || '0', like_volue || '0', introduction || '', video_time || '', description || '', order || 0]
    )

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'create_carousel', 'carousel', result.rows[0].id, `创建轮播图: ${title}`]
    )

    return { success: true, data: result.rows[0] }
  }

  throw createError({ statusCode: 405 })
})
