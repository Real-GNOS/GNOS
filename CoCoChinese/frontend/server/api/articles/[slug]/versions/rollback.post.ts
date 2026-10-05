import { requireAuth } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { listArticleVersions, readArticleVersion } from '~/server/utils/articleRepo'

export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const slug = getRouterParam(event, 'slug') || ''
  const body = await readBody(event).catch(() => ({}))
  const sha = (body?.sha || '').toString().trim()
  const reason = (body?.reason || '').toString().slice(0, 500)

  if (!slug) throw createError({ statusCode: 400, message: '缺少 slug' })
  if (!/^[0-9a-f]{7,40}$/.test(sha)) throw createError({ statusCode: 400, message: '无效的提交标识' })

  const post = await queryPg(
    'SELECT id, title, author FROM posts WHERE slug = $1',
    [slug]
  )
  if (!post.rows.length) throw createError({ statusCode: 404, message: '文章不存在' })
  if (auth.role !== 'admin' && auth.username !== post.rows[0].author) {
    throw createError({ statusCode: 403, message: '只能回滚自己文章的历史版本' })
  }

  const versions = await listArticleVersions(slug)
  const target = versions.find((v) => v.sha.startsWith(sha))
  if (!target) throw createError({ statusCode: 404, message: '目标版本不存在' })

  const snapshot = await readArticleVersion(slug, target.sha)
  if (!snapshot) throw createError({ statusCode: 404, message: '目标版本内容不可读' })

  await queryPg(
    `UPDATE posts SET title = $1, content = $2, cover_image = $3, category = $4, tags = $5, status = $6, updated_at = CURRENT_TIMESTAMP
     WHERE slug = $7`,
    [snapshot.title, snapshot.content, snapshot.cover_image || '',
     snapshot.category || '', snapshot.tags || [], snapshot.status || 'draft', slug]
  )
  await queryPg(
    `INSERT INTO audit_logs (user_id, action, target_type, target_id, details)
     VALUES ($1, $2, $3, $4, $5)`,
    [parseInt(auth.userId, 10), 'rollback_article_version', 'post', slug,
     `文章 ${slug} 回滚到 ${target.sha.slice(0, 8)}${reason ? '：' + reason : ''}`]
  )
  return { success: true, sha: target.sha, message: target.message, snapshot: { title: snapshot.title, status: snapshot.status } }
})
