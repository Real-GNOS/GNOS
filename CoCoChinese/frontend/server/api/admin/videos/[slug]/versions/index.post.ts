import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { publishVersion } from '~/server/utils/videoRepo'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const slug = getRouterParam(event, 'slug') || ''
  const body = await readBody(event).catch(() => ({}))
  const message = (body?.message || '').toString().slice(0, 500)

  if (!slug) throw createError({ statusCode: 400, message: '缺少视频标识' })

  const video = await queryPg('SELECT id, title FROM videos WHERE slug = $1', [slug])
  if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

  const nextRes = await queryPg(
    'SELECT COALESCE(MAX(version), 0) + 1 AS next FROM video_versions WHERE video_slug = $1',
    [slug]
  )
  const version = parseInt(nextRes.rows[0].next, 10)

  try {
    const sha = await publishVersion(slug, message || `发布版本 v${version}`, version)
    await queryPg(
      `INSERT INTO video_versions (video_slug, version, commit_sha, tag, message, operator_id, operator_name)
       VALUES ($1, $2, $3, $4, $5, $6, $7)`,
      [slug, version, sha, `v${version}`, message || `发布版本 v${version}`,
       parseInt(auth.userId, 10), auth.username]
    )
    await queryPg(
      `INSERT INTO audit_logs (user_id, action, target_type, target_id, details)
       VALUES ($1, $2, $3, $4, $5)`,
      [parseInt(auth.userId, 10), 'publish_video_version', 'video', slug,
       `视频 ${slug} 发布版本 v${version}：${message}`]
    )
    return { success: true, version, sha }
  } catch (err: any) {
    console.error('publish version error:', err)
    throw createError({ statusCode: 500, message: `版本发布失败：${err.message || 'git 错误'}` })
  }
})
