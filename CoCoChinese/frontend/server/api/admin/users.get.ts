export default defineEventHandler(async (event) => {
  try {
    await requireAdmin(event)
    const users = await findAllUsers()
    return users
  } catch (err) {
    if (err.statusCode) throw err
    console.error(err)
    throw createError({ statusCode: 500, message: '服务器错误' })
  }
})
