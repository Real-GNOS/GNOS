import { requireAuth } from '~/server/utils/pg'
import { queryPg } from '~/server/utils/pg'
import { listArticleVersions } from '~/server/utils/articleRepo'

export default defineEventHandler(async (event) => {
  const auth = await requireAuth(event)
  const slug = getRouterParam(event, 'slug') || ''

  if (!slug) throw createError({ statusCode: 400, message: '缺少 slug' })
  const post = await queryPg('SELECT id, title, author, content, status FROM posts WHERE slug = $1', [slug])
  if (!post.rows.length) throw createError({ statusCode: 404, message: '文章不存在' })

  const [gitVersions, dbVersions] = await Promise.all([
    listArticleVersions(slug),
    queryPg(
      `SELECT version, commit_sha, tag, message, operator_name, created_at
       FROM article_versions WHERE article_slug = $1 ORDER BY version DESC`,
      [slug]
    ),
  ])

  const bySha = new Map(dbVersions.rows.map((r: any) => [r.commit_sha, r]))
  const versions = gitVersions.map((v) => ({
    sha: v.sha,
    message: v.message,
    date: v.date,
    tag: v.tag,
    operator_name: bySha.get(v.sha)?.operator_name || null,
    published_at: bySha.get(v.sha)?.created_at || null,
  }))

  return {
    success: true,
    slug,
    post: { id: post.rows[0].id, title: post.rows[0].title, author: post.rows[0].author, status: post.rows[0].status },
    canEdit: auth.role === 'admin' || auth.username === post.rows[0].author,
    versions,
    total: versions.length,
  }
})
