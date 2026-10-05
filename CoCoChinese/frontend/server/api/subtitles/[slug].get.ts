export default defineEventHandler(async (event) => {
  const slug = getRouterParam(event, 'slug')
  const { queryPg } = await import('../../utils/pg')

  const subtitles = await queryPg(
    'SELECT * FROM subtitles WHERE video_slug = $1 ORDER BY is_default DESC',
    [slug]
  )

  const audioTracks = await queryPg(
    'SELECT * FROM audio_tracks WHERE video_slug = $1 ORDER BY is_default DESC',
    [slug]
  )

  const chapters = await queryPg(
    'SELECT * FROM video_chapters WHERE video_slug = $1 ORDER BY start_time ASC',
    [slug]
  )

  return { subtitles: subtitles.rows, audioTracks: audioTracks.rows, chapters: chapters.rows }
})
