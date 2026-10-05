export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)
  if (!auth) return { banned: false, ban: null }
  // 管理员（含超级管理员）不受封禁限制，前端不视为被封禁
  if (auth.role === 'admin' || auth.role === 'super_admin') {
    return { banned: false, ban: null }
  }
  const { getActiveBan } = await import('~/server/utils/pg')
  const ban = await getActiveBan(parseInt(auth.userId))
  return { banned: !!ban, ban: ban || null }
})
