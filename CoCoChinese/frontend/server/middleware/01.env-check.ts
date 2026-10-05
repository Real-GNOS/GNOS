export default defineEventHandler(async (event) => {
  const path = getRequestURL(event).pathname

  if (path.startsWith('/api/setup') || path.startsWith('/setup') || path === '/api/home') {
    return
  }

  const envCheck = process.env.SETUP_COMPLETED
  if (envCheck !== 'true') {
    const fs = await import('fs')
    const path_mod = await import('path')
    const envExists = fs.existsSync(path_mod.default.resolve(process.cwd(), '.env'))

    if (!envExists) {
      if (path.startsWith('/api/')) {
        throw createError({ statusCode: 503, statusMessage: 'Setup required', message: '请先完成安装配置' })
      }
      return sendRedirect(event, '/setup')
    }
  }
})
