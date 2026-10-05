export default defineEventHandler(async (event) => {
  try {
    const slug = getRouterParam(event, 'slug')
    if (!slug) throw createError({ statusCode: 400, message: '缺少用户标识' })

    const user = await queryPg(
      `SELECT u.id, u.username, u.display_name, u.avatar_url, u.created_at,
              COALESCE(p.bio, '') bio, COALESCE(p.gender, '') gender,
              p.birthday, COALESCE(p."location", '') "location",
              COALESCE(p.website, '') website,
              COALESCE(p.followers, 0) followers,
              COALESCE(p."following", 0) "following",
              COALESCE(p.likes, 0) likes
       FROM users u
       LEFT JOIN user_profiles p ON p.user_id = u.id
       WHERE u.slug = $1 OR u.username = $1`, [slug])
    if (!user.rows.length) throw createError({ statusCode: 404, message: '用户不存在' })

    const profile = user.rows[0]
    const videos = await queryPg(
      `SELECT id, slug, title, image_url, video_time, watch_volue, like_volue, created_at
       FROM videos WHERE author = $1 ORDER BY created_at DESC LIMIT 20`,
      [profile.username])

    let isFollowing = false
    try {
      const auth = getAuthFromEvent(event)
      if (auth && parseInt(auth.userId) !== profile.id) {
        const f = await queryPg(
          'SELECT id FROM user_follows WHERE follower_id = $1 AND following_id = $2',
          [parseInt(auth.userId), profile.id])
        isFollowing = f.rows.length > 0
      }
    } catch {}

    // 公开标记：任何访客查看该用户资料时都能看到其是否被封禁（B站风格）
    const selfAuth = getAuthFromEvent(event)
    const isSelf = selfAuth && parseInt(selfAuth.userId) === profile.id
    const activeBan = await getActiveBan(profile.id)
    // 仅当用户本人查看自己时，才返回封禁原因等隐私详情
    let myBan = null
    if (isSelf && activeBan) {
      myBan = { type: activeBan.type, reason: activeBan.reason, expires_at: activeBan.expires_at }
    }
    return { success: true, profile, videos: videos.rows, isFollowing, banned: !!activeBan, myBan }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Profile error:', err)
    throw createError({ statusCode: 500, message: '获取用户信息失败' })
  }
})
