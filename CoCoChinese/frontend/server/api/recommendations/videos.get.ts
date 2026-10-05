export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)
  const { queryPg } = await import('../../utils/pg')

  let recommendations: any[] = []

  if (auth) {
    // Get user's watch history categories/tags for personalization
    const history = await queryPg(`
      SELECT DISTINCT v.category, unnest(v.tags) as tag
      FROM watch_history wh
      JOIN videos v ON v.id = wh.video_id
      WHERE wh.user_id = $1
      ORDER BY wh.watched_at DESC
      LIMIT 20
    `, [auth.userId])

    if (history.rows.length) {
      const tags = [...new Set(history.rows.map(r => r.tag).filter(Boolean))]
      const categories = [...new Set(history.rows.map(r => r.category).filter(Boolean))]

      if (tags.length) {
        const tagRecs = await queryPg(`
          SELECT v.*, COUNT(*) OVER() as total_count
          FROM videos v
          WHERE v.tags && $1 AND v.id NOT IN (
            SELECT video_id FROM watch_history WHERE user_id = $2
          )
          ORDER BY (CASE WHEN v.watch_volue ~ '^\\d+$' THEN v.watch_volue::INTEGER ELSE 0 END) DESC
          LIMIT 20
        `, [tags, auth.userId])
        recommendations.push(...tagRecs.rows)
      }

      if (categories.length && recommendations.length < 20) {
        const catRecs = await queryPg(`
          SELECT v.* FROM videos v
          WHERE v.category = ANY($1) AND v.id NOT IN (
            SELECT video_id FROM watch_history WHERE user_id = $2
          )
          ORDER BY RANDOM()
          LIMIT 10
        `, [categories, auth.userId])
        recommendations.push(...catRecs.rows)
      }
    }
  }

  // Fallback to popular videos
  if (!recommendations.length) {
    const popular = await queryPg(`
      SELECT v.* FROM videos v
      WHERE v.is_deleted = false OR v.is_deleted IS NULL
      ORDER BY (CASE WHEN v.watch_volue ~ '^\\d+$' THEN v.watch_volue::INTEGER ELSE 0 END) DESC
      LIMIT 30
    `)
    recommendations = popular.rows
  }

  return { recommendations: recommendations.slice(0, 30) }
})
