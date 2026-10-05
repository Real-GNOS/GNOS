export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  const [userCount, videoCount, commentCount, danmakuCount, viewTotal, todayNewUsers, todayNewVideos, todayNewComments, banCount, reportPending] = await Promise.all([
    queryPg('SELECT COUNT(*) FROM users'),
    queryPg('SELECT COUNT(*) FROM videos WHERE is_deleted IS NOT true'),
    queryPg('SELECT COUNT(*) FROM comments'),
    queryPg('SELECT COUNT(*) FROM danmaku'),
    queryPg("SELECT COALESCE(SUM(CASE WHEN watch_volue ~ '^[0-9]+$' THEN watch_volue::bigint ELSE 0 END), 0) AS total FROM videos"),
    queryPg("SELECT COUNT(*) FROM users WHERE created_at >= CURRENT_DATE"),
    queryPg("SELECT COUNT(*) FROM videos WHERE created_at >= CURRENT_DATE"),
    queryPg("SELECT COUNT(*) FROM comments WHERE created_at >= CURRENT_DATE"),
    queryPg("SELECT COUNT(*) FROM user_bans WHERE is_active = true AND (expires_at IS NULL OR expires_at > NOW())"),
    queryPg("SELECT COUNT(*) FROM moderation_reports WHERE status = 'pending'").catch(() => ({ rows: [{ count: 0 }] })),
  ])

  const [recentUsers, recentVideos, topVideos, categoryStats] = await Promise.all([
    queryPg('SELECT id, username, display_name, avatar_url, role, created_at FROM users ORDER BY created_at DESC LIMIT 5'),
    queryPg('SELECT id, slug, title, author, image_url, category, watch_volue, like_volue, created_at, is_deleted FROM videos ORDER BY created_at DESC LIMIT 5'),
    queryPg("SELECT id, slug, title, author, watch_volue, like_volue, image_url FROM videos WHERE is_deleted IS NOT true ORDER BY CAST(watch_volue AS BIGINT) DESC NULLS LAST LIMIT 5"),
    queryPg("SELECT COALESCE(category, '未分类') AS category, COUNT(*) AS count FROM videos WHERE is_deleted IS NOT true GROUP BY category ORDER BY count DESC LIMIT 8"),
  ])

  const last7Days = await queryPg(`
    SELECT DATE(created_at) AS day, COUNT(*) AS count
    FROM users WHERE created_at >= CURRENT_DATE - INTERVAL '6 days'
    GROUP BY DATE(created_at) ORDER BY day
  `)

  const videoLast7Days = await queryPg(`
    SELECT DATE(created_at) AS day, COUNT(*) AS count
    FROM videos WHERE created_at >= CURRENT_DATE - INTERVAL '6 days'
    GROUP BY DATE(created_at) ORDER BY day
  `)

  return {
    stats: {
      totalUsers: parseInt(userCount.rows[0].count),
      totalVideos: parseInt(videoCount.rows[0].count),
      totalComments: parseInt(commentCount.rows[0].count),
      totalDanmaku: parseInt(danmakuCount.rows[0].count),
      totalViews: parseInt(viewTotal.rows[0].total),
      todayNewUsers: parseInt(todayNewUsers.rows[0].count),
      todayNewVideos: parseInt(todayNewVideos.rows[0].count),
      todayNewComments: parseInt(todayNewComments.rows[0].count),
      activeBans: parseInt(banCount.rows[0].count),
      pendingReports: parseInt(reportPending.rows[0].count),
    },
    recentUsers: recentUsers.rows,
    recentVideos: recentVideos.rows,
    topVideos: topVideos.rows,
    categoryStats: categoryStats.rows,
    userGrowth: last7Days.rows,
    videoGrowth: videoLast7Days.rows,
  }
})
