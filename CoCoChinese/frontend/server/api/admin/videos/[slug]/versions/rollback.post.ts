import { requireAdmin } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { rollbackTo, listVersions } from '~/server/utils/videoRepo'

export default defineEventHandler(async (event) => {
  const auth = await requireAdmin(event)
  const slug = getRouterParam(event, 'slug') || ''
  const body = await readBody(event).catch(() => ({}))
  const sha = (body?.sha || '').toString().trim()
  const reason = (body?.reason || '').toString().slice(0, 500)

  if (!slug) throw createError({ statusCode: 400, message: '缺少视频标识' })
  if (!/^[0-9a-f]{7,40}$/.test(sha)) {
    throw createError({ statusCode: 400, message: '无效的提交标识' })
  }

  const video = await queryPg('SELECT id, title FROM videos WHERE slug = $1', [slug])
  if (!video.rows.length) throw createError({ statusCode: 404, message: '视频不存在' })

  const versions = await listVersions(slug)
  const target = versions.find((v) => v.sha.startsWith(sha))
  if (!target) throw createError({ statusCode: 404, message: '目标版本不存在' })

  try {
    await rollbackTo(slug, target.sha)

    const log = await queryPg(
      `INSERT INTO audit_logs (user_id, action, target_type, target_id, details)
       VALUES ($1, $2, $3, $4, $5)
       RETURNING id`,
      [parseInt(auth.userId, 10), 'rollback_video_version', 'video', slug,
       `视频 ${slug} 回滚到 ${target.sha.slice(0, 8)}${reason ? '：' + reason : ''}`]
    )
    return { success: true, sha: target.sha, message: target.message, audit_id: log.rows[0]?.id }
  } catch (err: any) {
    console.error('rollback error:', err)
    throw createError({ statusCode: 500, message: `回滚失败：${err.message || 'git 错误'}` })
  }
})
