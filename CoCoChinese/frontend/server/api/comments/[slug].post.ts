export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const slug = getRouterParam(event, 'slug')
    if (!slug) throw createError({ statusCode: 400, message: '缺少参数' })
    const body = await readBody(event)
    if (!body || !body.content || !body.content.trim()) {
      throw createError({ statusCode: 400, message: '评论内容不能为空' })
    }
    const comment = await addComment(
      slug, parseInt(auth.userId), auth.username,
      auth.avatar_url || '/images/default_avatar.png',
      body.content.trim(),
      body.parent_id ? parseInt(body.parent_id) : undefined,
      body.images || []
    )

    try {
      const video = await queryPg('SELECT author, title FROM videos WHERE slug = $1', [slug])
      if (video.rows.length) {
        const authorUser = await queryPg(
          'SELECT u.id FROM users u WHERE u.username = $1', [video.rows[0].author])
        if (authorUser.rows.length && parseInt(authorUser.rows[0].id) !== parseInt(auth.userId)) {
          const authorId = parseInt(authorUser.rows[0].id)
          const truncatedContent = body.content.trim().slice(0, 80)

          await queryPg(
            `INSERT INTO messages (from_user_id, to_user_id, content, message_type)
             VALUES ($1, $2, $3, 'reply')`,
            [parseInt(auth.userId), authorId,
             `${auth.username} 回复了你的视频《${video.rows[0].title}》: ${truncatedContent}`]
          )

          await queryPg(
            `INSERT INTO notifications (user_id, type, title, body, from_user_id)
             VALUES ($1, 'comment', $2, $3, $4)`,
            [authorId,
             `${auth.username} 评论了你的视频`,
             `${auth.username} 在《${video.rows[0].title}》下评论: ${truncatedContent}`,
             parseInt(auth.userId)]
          )
        }

        if (body.parent_id) {
          const parentComment = await queryPg(
            'SELECT user_id, username FROM comments WHERE id = $1', [parseInt(body.parent_id)])
          if (parentComment.rows.length && parseInt(parentComment.rows[0].user_id) !== parseInt(auth.userId)) {
            const parentUserId = parseInt(parentComment.rows[0].user_id)
            await queryPg(
              `INSERT INTO notifications (user_id, type, title, body, from_user_id)
               VALUES ($1, 'reply', $2, $3, $4)`,
              [parentUserId,
               `${auth.username} 回复了你的评论`,
               `${auth.username} 在《${video.rows[0].title}》中回复了你: ${body.content.trim().slice(0, 80)}`,
               parseInt(auth.userId)]
            )
          }
        }
      }
    } catch (e) {
      console.error('Failed to send notification:', e)
    }

    return { success: true, data: comment }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Add comment error:', err)
    throw createError({ statusCode: 500, message: '评论失败' })
  }
})
