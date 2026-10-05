export default defineEventHandler(async (event) => {
  try {
    const auth = await requireAuth(event)
    const body = await readBody(event)
    if (!body || !body.to_user_id) throw createError({ statusCode: 400, message: '参数错误' })
    const content = (body.content || '').trim()
    const imageUrl = (body.imageUrl || '').trim()
    if (!content && !imageUrl) throw createError({ statusCode: 400, message: '内容不能为空' })
    const result = await queryPg(
      `INSERT INTO messages (from_user_id, to_user_id, content, image_url, message_type)
       VALUES ($1, $2, $3, $4, $5) RETURNING id, created_at`,
      [parseInt(auth.userId), body.to_user_id, content, imageUrl, body.message_type || 'dm']
    )
    const newMsg = {
      id: result.rows[0].id,
      from_user_id: parseInt(auth.userId),
      from_username: auth.username,
      from_avatar: auth.avatar_url || '',
      to_user_id: body.to_user_id,
      content,
      image_url: imageUrl,
      message_type: body.message_type || 'dm',
      is_read: false,
      created_at: result.rows[0].created_at,
    }
    try {
      const { publishChatMessage } = await import('../../utils/chat-events')
      publishChatMessage(newMsg)
    } catch {}
    return { success: true, message: '发送成功', data: newMsg }
  } catch (err) {
    if (err.statusCode) throw err
    console.error('Send message error:', err)
    throw createError({ statusCode: 500, message: '发送失败' })
  }
})
