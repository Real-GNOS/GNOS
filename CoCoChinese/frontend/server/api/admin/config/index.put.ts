export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const body = await readBody(event)
  const { queryPg } = await import('../../../utils/pg')

  for (const [key, value] of Object.entries(body)) {
    await queryPg(`
      INSERT INTO site_configs (key, value) VALUES ($1, $2)
      ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value
    `, [key, String(value)])
  }

  // Log audit
  await queryPg(
    'INSERT INTO audit_logs (user_id, action, target_type, details) VALUES ($1, $2, $3, $4)',
    [auth.userId, 'update_config', 'site_config', `Updated ${Object.keys(body).length} config(s)`]
  )

  return { success: true }
})
