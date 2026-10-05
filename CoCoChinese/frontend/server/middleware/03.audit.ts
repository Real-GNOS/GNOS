const AUDIT_METHODS = ['POST', 'PUT', 'PATCH', 'DELETE']

export default defineEventHandler(async (event) => {
  const method = getMethod(event)
  if (!AUDIT_METHODS.includes(method)) return

  const path = getRequestURL(event).pathname

  if (!path.startsWith('/api/admin/')) return

  try {
    const auth = getAuthFromEvent(event)
    if (!auth) return

    const action = `${method} ${path}`
    const { queryPg } = await import('../utils/pg')
    const body = await readBody(event).catch(() => null)

    const details = body
      ? JSON.stringify(body).substring(0, 500)
      : ''

    await queryPg(
      'INSERT INTO audit_logs (user_id, action, target_type, target_id, details, ip_address) VALUES ($1, $2, $3, $4, $5, $6)',
      [
        auth.userId,
        action,
        path.split('/')[3] || null,
        body?.id || null,
        details,
        getRequestIP(event, { xForwardedFor: true }) || null,
      ]
    )
  } catch {
    // Silent fail for audit logging
  }
})
