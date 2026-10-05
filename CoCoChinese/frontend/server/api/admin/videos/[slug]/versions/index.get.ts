import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { listVersions } from '~/server/utils/videoRepo'

export default defineEventHandler(async (event) => {
  await requireAdmin(event)
  const slug = getRouterParam(event, 'slug') || ''

  if (!slug) throw createError({ statusCode: 400, message: '缺少视频标识' })

  const video = await queryPg('SELECT id, title FROM videos WHERE slug = $1', [slug])
  if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

  const [gitVersions, dbVersions] = await Promise.all([
    listVersions(slug),
    queryPg(
      `SELECT version, commit_sha, tag, message, operator_name, created_at
       FROM video_versions WHERE video_slug = $1 ORDER BY version DESC`,
      [slug]
    ),
  ])

  // git 是权威的提交清单；数据库行只补充操作者与发布时间。
  const bySha = new Map(dbVersions.rows.map((r: any) => [r.commit_sha, r]))
  const versions = gitVersions.map((v) => ({
    sha: v.sha,
    message: v.message,
    date: v.date,
    tag: v.tag,
    operator_name: bySha.get(v.sha)?.operator_name || null,
    published_at: bySha.get(v.sha)?.created_at || null,
  }))

  return { success: true, slug, video: video.rows[0], versions, total: versions.length }
})
