import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  if (event.method === 'GET') {
    const query = getQuery(event)
    const videoId = parseInt(query.video_id as string)

    if (!videoId) throw createError({ statusCode: 400, message: '缺少 video_id' })

    const result = await queryPg(
      'SELECT * FROM video_episodes WHERE video_id = $1 ORDER BY season ASC, sort_order ASC, episode ASC',
      [videoId]
    )

    return { success: true, episodes: result.rows }
  }

  if (event.method === 'POST') {
    const body = await readBody(event)
    const { video_id, season, episode, title, video_url, image_url, duration, sort_order } = body

    if (!video_id || !title || !video_url) {
      throw createError({ statusCode: 400, message: '缺少必要参数' })
    }

    const result = await queryPg(
      `INSERT INTO video_episodes (video_id, season, episode, title, video_url, image_url, duration, sort_order)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8) RETURNING *`,
      [video_id, season || 1, episode || 1, title, video_url, image_url || '', duration || '', sort_order || 0]
    )

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details) VALUES ($1, $2, $3, $4, $5)',
      [auth.userId, 'create_episode', 'episode', result.rows[0].id, `创建剧集: ${title}`]
    )

    return { success: true, data: result.rows[0] }
  }

  throw createError({ statusCode: 405 })
})
