import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'

export default defineEventHandler(async (event) => {
  await requireAdmin(event)

  try {
    const result = await queryPg(`
      SELECT
        d::date as date,
        COALESCE((
          SELECT COALESCE(SUM(CASE WHEN v.watch_volue ~ '^\\d+$' THEN v.watch_volue::integer ELSE 0 END), 0)
          FROM videos v
          WHERE v.created_at::date = d::date
        ), 0) as views,
        COALESCE((
          SELECT count(*) FROM videos WHERE created_at::date = d::date
        ), 0) as uploads,
        COALESCE((
          SELECT count(*) FROM users WHERE created_at::date = d::date
        ), 0) as registrations
      FROM generate_series(
        CURRENT_DATE - INTERVAL '6 days',
        CURRENT_DATE,
        '1 day'
      ) d
      ORDER BY d
    `)

    return { success: true, trends: result.rows }
  } catch (err) {
    console.error('Dashboard trends error:', err)
    throw createError({ statusCode: 500, message: '获取趋势数据失败' })
  }
})
