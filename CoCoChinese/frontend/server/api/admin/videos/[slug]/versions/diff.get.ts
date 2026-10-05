import { requireAdmin } from '~/server/utils/pg'
import { diffVersions } from '~/server/utils/videoRepo'

export default defineEventHandler(async (event) => {
  await requireAdmin(event)
  const slug = getRouterParam(event, 'slug') || ''
  const { from, to } = getQuery(event)

  if (!slug) throw createError({ statusCode: 400, message: '缺少视频标识' })
  if (typeof from !== 'string' || typeof to !== 'string' ||
      !/^[0-9a-f]{7,40}$/.test(from) || !/^[0-9a-f]{7,40}$/.test(to)) {
    throw createError({ statusCode: 400, message: 'from/to 参数无效' })
  }

  try {
    return { success: true, slug, from, to, ...(await diffVersions(slug, from, to)) }
  } catch (err: any) {
    console.error('diff error:', err)
    throw createError({ statusCode: 500, message: `对比失败：${err.message || 'git 错误'}` })
  }
})
