import { requireAuth } from '~/server/utils/pg'
import { diffArticleVersions } from '~/server/utils/articleRepo'

export default defineEventHandler(async (event) => {
  await requireAuth(event)
  const slug = getRouterParam(event, 'slug') || ''
  const { from, to } = getQuery(event)

  if (!slug) throw createError({ statusCode: 400, message: '缺少 slug' })
  if (typeof from !== 'string' || typeof to !== 'string' ||
      !/^[0-9a-f]{7,40}$/.test(from) || !/^[0-9a-f]{7,40}$/.test(to)) {
    throw createError({ statusCode: 400, message: 'from/to 参数无效' })
  }

  try {
    const { diff, summary } = await diffArticleVersions(slug, from, to)
    return { success: true, slug, from, to, diff, summary }
  } catch (err: any) {
    console.error('article diff error:', err)
    throw createError({ statusCode: 500, message: `对比失败：${err.message || 'git 错误'}` })
  }
})
