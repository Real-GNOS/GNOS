import { requireAuth, queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const targetId = parseInt(getRouterParam(event, 'id') || '')
    if (isNaN(targetId)) throw createError({ statusCode: 400, message: '无效ID' })
    if (parseInt(auth.userId) === targetId) throw createError({ statusCode: 400, message: '不能关注自己' })

    const existing = await queryPg(
      'SELECT id FROM user_follows WHERE follower_id = $1 AND following_id = $2',
      [parseInt(auth.userId), targetId]
    )

    if (existing.rows.length) {
      await queryPg('DELETE FROM user_follows WHERE follower_id = $1 AND following_id = $2',
        [parseInt(auth.userId), targetId])
      await queryPg('UPDATE user_profiles SET followers = GREATEST(followers - 1, 0) WHERE user_id = $1', [targetId])
      await queryPg('UPDATE user_profiles SET "following" = GREATEST("following" - 1, 0) WHERE user_id = $1', [parseInt(auth.userId)])
      return { following: false }
    } else {
      await queryPg('INSERT INTO user_follows (follower_id, following_id) VALUES ($1, $2)',
        [parseInt(auth.userId), targetId])
      await queryPg('UPDATE user_profiles SET followers = followers + 1 WHERE user_id = $1', [targetId])
      await queryPg('UPDATE user_profiles SET "following" = "following" + 1 WHERE user_id = $1', [parseInt(auth.userId)])
      await queryPg(
        'INSERT INTO messages (from_user_id, to_user_id, content, message_type) VALUES ($1, $2, $3, $4)',
        [parseInt(auth.userId), targetId, `${auth.username} 关注了你`, 'system']
      )
      await queryPg(
        `INSERT INTO notifications (user_id, type, title, body, from_user_id)
         VALUES ($1, 'follow', $2, $3, $4)`,
        [targetId, `${auth.username} 关注了你`, `${auth.username} 开始关注你的动态`, parseInt(auth.userId)]
      )
      return { following: true }
    }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Follow error:', err)
    throw createError({ statusCode: 500, message: '操作失败' })
  }
})
