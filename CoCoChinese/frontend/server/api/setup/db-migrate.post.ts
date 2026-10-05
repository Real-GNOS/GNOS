export default defineEventHandler(async () => {
  try {
    const { ensureDatabase, initPgTables } = await import('../../utils/pg')
    await ensureDatabase()
    await initPgTables()
    return { success: true, message: '数据库已初始化' }
  } catch (e: any) {
    throw createError({ statusCode: 500, message: `数据库初始化失败: ${e.message}` })
  }
})
