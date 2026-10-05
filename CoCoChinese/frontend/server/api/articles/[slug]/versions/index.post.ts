import { requireAuth } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { commitArticleVersion } from '~/server/utils/articleRepo'

export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const slug = getRouterParam(event, 'slug') || ''
  const body = await readBody(event).catch(() => ({}))
  const message = (body?.message || '').toString().slice(0, 500)

  if (!slug) throw createError({ statusCode: 400, message: '缺少 slug' })
  const post = await queryPg(
    'SELECT id, title, content, cover_image, category, tags, status, author FROM posts WHERE slug = $1',
    [slug]
  )
  if (!post.rows.length) throw createError({ statusCode: 404, message: '文章不存在' })
  if (auth.role !== 'admin' && auth.username !== post.rows[0].author) {
    throw createError({ statusCode: 403, message: '只能提交自己文章的新版本' })
  }

  const row = post.rows[0]
  const next = await queryPg(
    'SELECT COALESCE(MAX(version), 0) + 1 AS next FROM article_versions WHERE article_slug = $1',
    [slug]
  )
  const version = parseInt(next.rows[0].next, 10)

  const sha = await commitArticleVersion(slug, {
    title: row.title,
    content: row.content,
    cover_image: row.cover_image,
    category: row.category,
    tags: row.tags,
    status: row.status,
  }, message || `保存版本 v${version}`, version)

  await queryPg(
    `INSERT INTO article_versions (article_slug, version, commit_sha, tag, message, operator_id, operator_name)
     VALUES ($1, $2, $3, $4, $5, $6, $7)`,
    [slug, version, sha, `v${version}`, message || `保存版本 v${version}`,
     parseInt(auth.userId, 10), auth.username]
  )
  await queryPg(
    `INSERT INTO audit_logs (user_id, action, target_type, target_id, details)
     VALUES ($1, $2, $3, $4, $5)`,
    [parseInt(auth.userId, 10), 'commit_article_version', 'post', slug,
     `文章 ${slug} 提交版本 v${version}`]
  )
  return { success: true, version, sha }
})
