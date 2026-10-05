import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  await requireAdmin(event)

  try {
    const [
      users, videos, comments, bans,
      todayUsers, todayVideos, todayComments
    ] = await Promise.all([
      queryPg('SELECT count(*) FROM users'),
      queryPg('SELECT count(*) FROM videos'),
      queryPg('SELECT count(*) FROM comments'),
      queryPg('SELECT count(*) FROM user_bans WHERE is_active = true AND (expires_at IS NULL OR expires_at > NOW())'),
      queryPg("SELECT count(*) FROM users WHERE created_at >= CURRENT_DATE"),
      queryPg("SELECT count(*) FROM videos WHERE created_at >= CURRENT_DATE"),
      queryPg("SELECT count(*) FROM comments WHERE created_at >= CURRENT_DATE"),
    ])

    return {
      success: true,
      stats: {
        totalUsers: parseInt(users.rows[0].count),
        totalVideos: parseInt(videos.rows[0].count),
        totalComments: parseInt(comments.rows[0].count),
        activeBans: parseInt(bans.rows[0].count),
        todayUsers: parseInt(todayUsers.rows[0].count),
        todayVideos: parseInt(todayVideos.rows[0].count),
        todayComments: parseInt(todayComments.rows[0].count),
      }
    }
  } catch (err) {
    console.error('Admin stats error:', err)
    throw createError({ statusCode: 500, message: '获取统计失败' })
  }
})