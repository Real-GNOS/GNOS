import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const query = getQuery(event)

  const page = Math.max(1, parseInt(query.page as string) || 1)
  const size = Math.min(100, Math.max(1, parseInt(query.size as string) || 20))
  const offset = (page - 1) * size
  const search = (query.search as string) || ''
  const role = (query.role as string) || ''
  const status = (query.status as string) || ''

  let where = ''
  const params: any[] = []

  if (search) {
    where = 'WHERE (u.username ILIKE $1 OR u.display_name ILIKE $1 OR u.email ILIKE $1)'
    params.push(`%${search}%`)
  }

  if (role) {
    const idx = params.length + 1
    where = where ? `${where} AND u.role = $${idx}` : `WHERE u.role = $${idx}`
    params.push(role)
  }

  if (status === 'banned') {
    where = where
      ? `${where} AND EXISTS (SELECT 1 FROM user_bans ub WHERE ub.user_id = u.id AND ub.is_active = true AND (ub.expires_at IS NULL OR ub.expires_at > NOW()))`
      : `WHERE EXISTS (SELECT 1 FROM user_bans ub WHERE ub.user_id = u.id AND ub.is_active = true AND (ub.expires_at IS NULL OR ub.expires_at > NOW()))`
  } else if (status === 'normal') {
    where = where
      ? `${where} AND NOT EXISTS (SELECT 1 FROM user_bans ub WHERE ub.user_id = u.id AND ub.is_active = true AND (ub.expires_at IS NULL OR ub.expires_at > NOW()))`
      : `WHERE NOT EXISTS (SELECT 1 FROM user_bans ub WHERE ub.user_id = u.id AND ub.is_active = true AND (ub.expires_at IS NULL OR ub.expires_at > NOW()))`
  }

  const countResult = await queryPg(`SELECT count(*) FROM users u ${where}`, params)
  const total = parseInt(countResult.rows[0].count)

  params.push(size, offset)
  const result = await queryPg(
    `SELECT u.id, u.slug, u.username, u.display_name, u.email, u.role, u.avatar_url, u.created_at, u.updated_at,
            (SELECT json_build_object('id', ub.id, 'type', ub.type, 'reason', ub.reason, 'expires_at', ub.expires_at, 'is_active', ub.is_active)
             FROM user_bans ub
             WHERE ub.user_id = u.id AND ub.is_active = true AND (ub.expires_at IS NULL OR ub.expires_at > NOW())
             LIMIT 1) as active_ban
     FROM users u
     ${where}
     ORDER BY u.created_at DESC
     LIMIT $${params.length - 1} OFFSET $${params.length}`,
    params
  )

  return { success: true, total, page, size, users: result.rows }
})
