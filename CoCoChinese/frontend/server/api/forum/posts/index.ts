import { requireAuth } from '../../../utils/pg'
import { createForumPost } from '../../../utils/forum'

export default defineEventHandler(async (event) => {
  if (event.method !== 'POST') {
    throw createError({ statusCode: 405, message: 'Method not allowed' })
  }

  const auth = await requireAuth(event)
  const body = await readBody(event)
  if (!body || !body.topic_id || !body.content) {
    throw createError({ statusCode: 400, message: '缺少必要参数' })
  }

  const post = await createForumPost({
    topic_id: parseInt(body.topic_id),
    author_id: parseInt(auth.userId),
    content: body.content.trim(),
  })

  return { success: true, data: post }
})
