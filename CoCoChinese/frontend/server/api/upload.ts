export default defineEventHandler(async () => {
  throw createError({ statusCode: 405, message: '请使用 POST 方法上传' })
})
