import path from 'path'
import fs from 'fs/promises'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)

  const body = await readBody(event).catch(() => ({}))
  const slug = body?.slug as string | undefined

  const { queryPg } = await import('~/server/utils/pg')
  const { transcodeAllResolutions } = await import('~/server/utils/media')

  const storageDir = path.resolve('storage/videos')

  let videos: any[]
  if (slug) {
    const r = await queryPg('SELECT slug, video_url, video_url_hls FROM videos WHERE slug = $1', [slug])
    videos = r.rows
    if (!videos.length) throw createError({ statusCode: 404, message: `视频 ${slug} 不存在` })
  } else {
    const r = await queryPg('SELECT slug, video_url, video_url_hls FROM videos WHERE video_url_hls IS NOT NULL AND video_url_hls != \'\'')
    videos = r.rows
  }

  const results: { slug: string; ok: boolean; error?: string }[] = []

  for (const v of videos) {
    const s = v.slug
    const mp4Path = path.join(storageDir, `${s}.mp4`)

    try {
      const stat = await fs.stat(mp4Path)
      if (!stat.isFile()) throw new Error('MP4 not found')
    } catch {
      results.push({ slug: s, ok: false, error: 'MP4 file not found' })
      continue
    }

    try {
      const hlsDir = path.join(storageDir, s)
      const subDirs = ['360p', '720p', '1080p']
      for (const d of subDirs) {
        const dir = path.join(hlsDir, d)
        const files = await fs.readdir(dir).catch(() => [])
        for (const f of files) {
          if (f.endsWith('.ts') || f.endsWith('.m3u8')) {
            await fs.unlink(path.join(dir, f)).catch(() => {})
          }
        }
      }
      const masterPath = path.join(hlsDir, 'master.m3u8')
      await fs.unlink(masterPath).catch(() => {})

      await transcodeAllResolutions(mp4Path, storageDir, s)
      results.push({ slug: s, ok: true })
      console.log(`[retranscode] ${s} done`)
    } catch (err: any) {
      results.push({ slug: s, ok: false, error: err.message })
      console.error(`[retranscode] ${s} failed:`, err.message)
    }
  }

  const ok = results.filter(r => r.ok).length
  const fail = results.filter(r => !r.ok).length
  return { success: true, total: results.length, ok, fail, results }
})
