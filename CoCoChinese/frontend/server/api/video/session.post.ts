export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)
  if (!auth) {
    throw createError({ statusCode: 401, message: '请先登录' })
  }
  const sessionId = createSession()
  const key = getSessionKey(sessionId)
  return { success: true, sessionId, key: key?.toString('hex') || '', expiresIn: 3600 }
})
