export default defineEventHandler(async (event) => {
  const auth = getAuthFromEvent(event)
  const { queryPg } = await import('../../utils/pg')

  let playlists
  if (auth) {
    playlists = await queryPg(
      'SELECT * FROM playlists WHERE user_id = $1 OR (is_public = true) ORDER BY created_at DESC',
      [auth.userId]
    )
  } else {
    playlists = await queryPg(
      'SELECT * FROM playlists WHERE is_public = true ORDER BY created_at DESC'
    )
  }

  return { playlists: playlists.rows }
})
