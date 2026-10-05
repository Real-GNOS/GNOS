export default defineEventHandler(async (event) => {
  const { queryPg } = await import('../../utils/pg')

  const liveStreams = await queryPg(
    `SELECT s.*, u.username, u.avatar_url
     FROM streams s
     JOIN users u ON u.id = s.user_id
     WHERE s.status = 'live'
     ORDER BY s.viewer_count DESC`
  )

  return { streams: liveStreams.rows }
})
