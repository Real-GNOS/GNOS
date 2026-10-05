export default defineEventHandler(async () => {
  try {
    const items = await prisma.category.findMany({ orderBy: { order: 'asc' } })
    return items
  } catch (err: any) {
    console.error(err)
    throw createError({ statusCode: 500, message: '服务器错误' })
  }
})
